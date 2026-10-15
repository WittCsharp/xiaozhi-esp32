#!/usr/bin/env python3
"""Mac-side stub for Xiaozhi body mode.

The device POSTs JSON to /body/event and a length-prefixed Opus clip to
/body/audio. This process logs both. It does not run speech recognition.

TODO: hand the utterance to the Grok Bot, then speak the reply. Until then,
pass --reply to call scripts/lan_control/mac_speak.py, or copy the printed
command.

Audio body, repeated until the end:
    uint16 little-endian payload length, then that many raw Opus bytes.
Sample rate, channel count, and frame duration are HTTP headers.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Optional


MAX_BODY_BYTES = 256 * 1024
MAC_SPEAK = Path(__file__).resolve().parent / "mac_speak.py"


def parse_opus_frames(blob: bytes) -> list[bytes]:
    """Split a device clip into raw Opus packets."""
    frames: list[bytes] = []
    offset = 0
    while offset < len(blob):
        if offset + 2 > len(blob):
            raise ValueError("truncated opus header")
        size = blob[offset] | (blob[offset + 1] << 8)
        offset += 2
        if offset + size > len(blob):
            raise ValueError("truncated opus frame")
        frames.append(bytes(blob[offset : offset + size]))
        offset += size
    return frames


def speak_command(device: str, token: str, text: str, emotion: str = "thinking") -> list[str]:
    return [
        sys.executable,
        str(MAC_SPEAK),
        "--device",
        device,
        "--token",
        token,
        "--text",
        text,
        "--emotion",
        emotion,
    ]


def placeholder_text(frame_count: int) -> str:
    return (
        f"Body bridge received {frame_count} opus frames. "
        "ASR is not implemented."
    )


class BridgeConfig:
    def __init__(self, device: str, token: str, reply: Optional[str]) -> None:
        self.device = device
        self.token = token
        self.reply = reply
        self.clips = 0


def handle_uplink(path: str, headers: dict[str, str], payload: bytes, config: BridgeConfig) -> tuple[int, dict]:
    """Return an HTTP status and JSON body for one uplink. Logs to stdout."""
    route = urllib.parse.urlsplit(path).path
    if route == "/body/event":
        try:
            event = json.loads(payload.decode("utf-8")) if payload else {}
        except (UnicodeDecodeError, json.JSONDecodeError):
            return 400, {"ok": False, "error": "invalid_json"}
        if not isinstance(event, dict):
            return 400, {"ok": False, "error": "invalid_json"}
        print(
            "[body] event {event} source={source} device_id={device_id} ip={ip}".format(
                event=event.get("event", ""),
                source=event.get("source", ""),
                device_id=event.get("device_id", ""),
                ip=event.get("ip", ""),
            )
        )
        return 200, {"ok": True}

    if route != "/body/audio":
        return 404, {"ok": False, "error": "not_found"}

    try:
        frames = parse_opus_frames(payload)
    except ValueError as exc:
        print(f"[body] audio rejected: {exc}")
        return 400, {"ok": False, "error": "invalid_audio"}

    config.clips += 1
    source = headers.get("x-source", "")
    sample_rate = headers.get("x-sample-rate", "")
    frame_ms = headers.get("x-frame-duration-ms", "")
    device_id = headers.get("device-id", "")
    print(
        f"[body] audio clip={config.clips} bytes={len(payload)} frames={len(frames)} "
        f"rate={sample_rate} frame_ms={frame_ms} source={source} device_id={device_id}"
    )
    print(f"[body] ASR placeholder: not implemented ({len(frames)} frames).")
    print("[body] TODO: pass the transcript to the Grok Bot.")
    text = config.reply if config.reply else placeholder_text(len(frames))
    if config.device and config.token:
        command = speak_command(config.device, config.token, text)
        if config.reply:
            print("[body] speaking reply via mac_speak.py")
            subprocess.run(command, check=False)
        else:
            print("[body] mac_speak command:")
            print(" ".join(command))
    else:
        print("[body] pass --device and --token to print a mac_speak command")
    return 200, {"ok": True, "frames": len(frames), "bytes": len(payload)}


def make_handler(config: BridgeConfig):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:  # noqa: N802 - stdlib name
            route = urllib.parse.urlsplit(self.path).path
            if route != "/health":
                self._send(404, {"ok": False, "error": "not_found"})
                return
            self._send(200, {"ok": True, "role": "body_bridge"})

        def do_POST(self) -> None:  # noqa: N802 - stdlib name
            length = self.headers.get("Content-Length")
            if length is None:
                self._send(411, {"ok": False, "error": "length_required"})
                return
            try:
                size = int(length)
            except ValueError:
                self._send(400, {"ok": False, "error": "invalid_length"})
                return
            if size < 0 or size > MAX_BODY_BYTES:
                self._send(413, {"ok": False, "error": "payload_too_large"})
                return
            payload = self.rfile.read(size)
            header_map = {key.lower(): value for key, value in self.headers.items()}
            status, body = handle_uplink(self.path, header_map, payload, config)
            self._send(status, body)

        def _send(self, status: int, body: dict) -> None:
            raw = json.dumps(body).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(raw)

        def log_message(self, fmt: str, *args) -> None:
            print(f"[body] {self.address_string()} {fmt % args}")

    return Handler


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=9100)
    parser.add_argument("--device", default="", help="http://<device-ip>:8080 for mac_speak")
    parser.add_argument("--token", default="", help="LAN bearer token; printed only in the mac_speak command")
    parser.add_argument(
        "--reply",
        default="",
        help="If set, run mac_speak.py with this text after each audio clip",
    )
    args = parser.parse_args(argv)
    config = BridgeConfig(args.device, args.token, args.reply or None)
    try:
        server = ThreadingHTTPServer((args.host, args.port), make_handler(config))
    except OSError as exc:
        print(f"could not listen on {args.host}:{args.port}: {exc}", file=sys.stderr)
        return 1
    print(f"body bridge listening on http://{args.host}:{args.port}")
    print("POST /body/event and POST /body/audio")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("body bridge stopped")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
