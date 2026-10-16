#!/usr/bin/env python3
"""Play Mac speech on a Xiaozhi device through the LAN control API.

The device must already be on the same Wi-Fi network, idle, and built with
CONFIG_LAN_CONTROL_API. This script synthesizes a finished mono Ogg Opus file,
hosts it, and posts the URL. The device can play an Ogg response as the bytes
arrive, but this script does not open the URL until say and ffmpeg have
finished, so playback waits on the whole file. For body mode, stream Opus or
PCM with scripts/lan_control/stream_speak.py instead. This does not talk to
the cloud push API.

See docs/lan-control.md.
"""

from __future__ import annotations

import argparse
import json
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Optional


def ffmpeg_command(source: Path, dest: Path) -> list[str]:
    """Mono 16 kHz Ogg Opus at 20 ms frames. The device rejects stereo and redirects."""
    return [
        "ffmpeg",
        "-y",
        "-i",
        str(source),
        "-ar",
        "16000",
        "-ac",
        "1",
        "-c:a",
        "libopus",
        "-b:a",
        "24k",
        "-frame_duration",
        "20",
        "-vn",
        str(dest),
    ]


def say_command(text: str, dest: Path) -> list[str]:
    return ["say", "-o", str(dest), text]


def notify_payload(audio_url: str, subtitle: Optional[str]) -> dict:
    body: dict = {"audio_url": audio_url}
    if subtitle:
        body["subtitles"] = [{"start_ms": 0, "text": subtitle}]
    return body


def alert_payload(status: str, message: str, emotion: str) -> dict:
    return {"status": status, "message": message, "emotion": emotion}


def is_ogg(path: Path) -> bool:
    with path.open("rb") as handle:
        return handle.read(4) == b"OggS"


def device_base(device: str) -> tuple[str, str]:
    parsed = urllib.parse.urlparse(device)
    if parsed.scheme != "http" or not parsed.hostname:
        raise SystemExit("device must look like http://192.168.1.50:8080")
    host = parsed.hostname
    display_host = f"[{host}]" if ":" in host else host
    return f"http://{display_host}:{parsed.port or 8080}", host


def local_ip_toward(peer: str) -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.connect((peer, 9))
        return sock.getsockname()[0]


class _OggHandler(BaseHTTPRequestHandler):
    ogg = b""

    def do_GET(self) -> None:  # noqa: N802 - stdlib handler name
        path = urllib.parse.urlsplit(self.path).path
        if path != "/notify.ogg":
            body = b""
            status = 404
            content_type = "text/plain"
        else:
            body = type(self).ogg
            status = 200
            content_type = "audio/ogg"
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if body:
            self.wfile.write(body)

    def log_message(self, fmt: str, *args) -> None:
        print(f"[audio] {self.address_string()} {fmt % args}")


def _request(url: str, token: str, payload: Optional[dict], timeout: float) -> tuple[int, dict]:
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(url, data=data, method="GET" if data is None else "POST")
    request.add_header("Authorization", f"Bearer {token}")
    if data is not None:
        request.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read().decode("utf-8")
            return response.status, json.loads(raw) if raw else {}
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", errors="replace")
        try:
            parsed = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            parsed = {"error": raw}
        return exc.code, parsed


def _poll_until_idle(base: str, token: str, timeout_s: float) -> dict:
    deadline = time.monotonic() + timeout_s
    last: dict = {}
    saw_notifying = False
    idle_seen = 0
    while time.monotonic() < deadline:
        status, last = _request(f"{base}/api/health", token, None, 5)
        if status != 200:
            raise SystemExit(f"health failed: {status} {last}")
        state = last.get("state")
        if state == "notifying":
            saw_notifying = True
            idle_seen = 0
        elif saw_notifying:
            return last
        elif state == "idle":
            idle_seen += 1
            # StartNotification leaves idle immediately. A couple of idle polls
            # means playback never started or already finished.
            if idle_seen >= 4:
                return last
        else:
            idle_seen = 0
        time.sleep(0.5)
    print(f"stopped waiting; last health: {last}")
    return last


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, help="http://<device-ip>:8080")
    parser.add_argument("--token", required=True, help="bearer token; not printed")
    parser.add_argument("--text", help="sentence for macOS say")
    parser.add_argument("--ogg", type=Path, help="existing mono Ogg Opus file")
    parser.add_argument("--subtitle", help="on-screen line; defaults to --text")
    parser.add_argument("--emotion", help="display emotion, for example happy")
    parser.add_argument("--status", default="LAN", help="alert status text")
    parser.add_argument("--serve-port", type=int, default=8090)
    parser.add_argument("--timeout", type=float, default=90, help="seconds to wait for idle")
    parser.add_argument("--dry-run", action="store_true", help="print actions without network or TTS")
    args = parser.parse_args(argv)

    if not args.text and not args.ogg and not args.emotion:
        parser.error("pass --text, --ogg, or --emotion")
    if args.text and args.ogg:
        parser.error("pass only one of --text and --ogg")

    base, peer = device_base(args.device)
    subtitle = args.subtitle if args.subtitle is not None else args.text
    actions = []
    if args.emotion:
        message = subtitle or args.emotion
        actions.append(("POST", "/api/alert", alert_payload(args.status, message, args.emotion)))

    ogg_bytes = b""
    audio_url = ""
    if args.text or args.ogg:
        ip = "192.0.2.10" if args.dry_run else local_ip_toward(peer)
        audio_url = f"http://{ip}:{args.serve_port}/notify.ogg"
        actions.append(("POST", "/api/notify", notify_payload(audio_url, subtitle)))

    if args.dry_run:
        if args.text:
            print(" ".join(say_command(args.text, Path("/tmp/notify.aiff"))))
            print(" ".join(ffmpeg_command(Path("/tmp/notify.aiff"), Path("/tmp/notify.ogg"))))
        for method, path, payload in actions:
            print(f"{method} {base}{path}")
            print(json.dumps(payload, ensure_ascii=False))
        return 0

    if args.ogg:
        if not args.ogg.is_file() or not is_ogg(args.ogg):
            raise SystemExit(f"{args.ogg} is not an Ogg file")
        ogg_bytes = args.ogg.read_bytes()
    elif args.text:
        if shutil.which("say") is None or shutil.which("ffmpeg") is None:
            raise SystemExit("text mode needs macOS say and ffmpeg on PATH")
        with tempfile.TemporaryDirectory() as directory:
            aiff = Path(directory) / "notify.aiff"
            ogg = Path(directory) / "notify.ogg"
            subprocess.run(say_command(args.text, aiff), check=True)
            encoded = subprocess.run(ffmpeg_command(aiff, ogg), check=False)
            if encoded.returncode != 0 or not is_ogg(ogg):
                raise SystemExit("ffmpeg did not write a mono Ogg Opus file")
            ogg_bytes = ogg.read_bytes()

    server = None
    thread = None
    if ogg_bytes:
        _OggHandler.ogg = ogg_bytes
        try:
            server = ThreadingHTTPServer(("0.0.0.0", args.serve_port), _OggHandler)
        except OSError as exc:
            raise SystemExit(f"could not listen on port {args.serve_port}: {exc}") from exc
        thread = threading.Thread(
            target=server.serve_forever, kwargs={"poll_interval": 0.2}, daemon=True
        )
        thread.start()
        print(f"hosting {audio_url}")

    try:
        for method, path, payload in actions:
            status, body = _request(f"{base}{path}", args.token, payload, 10)
            print(f"{method} {path} -> {status} {json.dumps(body, ensure_ascii=False)}")
            if status >= 400:
                return 1
        if any(path == "/api/notify" for _, path, _ in actions):
            _poll_until_idle(base, args.token, args.timeout)
    finally:
        if server is not None:
            server.shutdown()
            server.server_close()
        if thread is not None:
            thread.join(timeout=2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
