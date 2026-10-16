#!/usr/bin/env python3
"""Push Opus or PCM to a Xiaozhi device while body mode is on.

The device plays each WebSocket frame as it arrives, so a bridge can send the
first TTS chunk before the rest of the utterance exists. POST /api/notify is
still the fallback when the only thing you have is a finished Ogg file.

esp_http_server does not decode a chunked request body, so this client uses
WebSocket frames on /api/speak instead of Transfer-Encoding: chunked.

Start text frame:
    {"event":"start","codec":"opus","sample_rate":16000,"channels":1,
     "frame_duration_ms":20,"framing":"packet","text":"Hello","emotion":"happy"}

Then binary frames:
    codec opus, framing packet: one raw Opus packet per frame
    codec opus, framing length-prefixed: uint16-le size + packet, repeated
    codec pcm: little-endian mono s16le at sample_rate

End with {"event":"end"}. The device stays "speaking" until the queued audio
drains; poll GET /api/health. {"event":"abort"} stops playback.

See docs/lan-control.md.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import socket
import urllib.parse
from pathlib import Path
from typing import Optional


OPUS = 0x2
TEXT = 0x1


def start_message(
    codec: str = "opus",
    sample_rate: int = 16000,
    frame_duration_ms: int = 60,
    framing: str = "packet",
    text: Optional[str] = None,
    emotion: Optional[str] = None,
    channels: int = 1,
) -> dict:
    body: dict = {
        "event": "start",
        "codec": codec,
        "sample_rate": sample_rate,
        "channels": channels,
    }
    if codec == "opus":
        body["frame_duration_ms"] = frame_duration_ms
        body["framing"] = framing
    if text is not None:
        body["text"] = text
    if emotion:
        body["emotion"] = emotion
    return body


def encode_client_frame(opcode: int, payload: bytes, mask_key: bytes) -> bytes:
    """One masked client frame with FIN set. mask_key is 4 bytes."""
    if len(mask_key) != 4:
        raise ValueError("mask_key must be 4 bytes")
    header = bytearray([0x80 | (opcode & 0x0F)])
    length = len(payload)
    if length < 126:
        header.append(0x80 | length)
    elif length < 65536:
        header.append(0x80 | 126)
        header.extend(length.to_bytes(2, "big"))
    else:
        header.append(0x80 | 127)
        header.extend(length.to_bytes(8, "big"))
    header.extend(mask_key)
    masked = bytes(byte ^ mask_key[index % 4] for index, byte in enumerate(payload))
    return bytes(header) + masked


def decode_server_frame(buf: bytes) -> Optional[tuple[int, bytes, bytes]]:
    """Parse one unmasked server frame. Returns opcode, payload, and the rest."""
    if len(buf) < 2:
        return None
    opcode = buf[0] & 0x0F
    if buf[1] & 0x80:
        raise ValueError("server frame is masked")
    length = buf[1] & 0x7F
    offset = 2
    if length == 126:
        if len(buf) < 4:
            return None
        length = int.from_bytes(buf[2:4], "big")
        offset = 4
    elif length == 127:
        if len(buf) < 10:
            return None
        length = int.from_bytes(buf[2:10], "big")
        offset = 10
    end = offset + length
    if len(buf) < end:
        return None
    return opcode, buf[offset:end], buf[end:]


def split_pcm(pcm: bytes, sample_rate: int, frame_ms: int) -> list[bytes]:
    """Split little-endian mono s16le into frame_ms chunks. The tail is kept."""
    if sample_rate <= 0 or frame_ms <= 0:
        raise ValueError("sample_rate and frame_ms must be positive")
    if len(pcm) % 2:
        raise ValueError("pcm length must be even")
    samples = max(1, sample_rate * frame_ms // 1000)
    stride = samples * 2
    if not pcm:
        return []
    return [pcm[index : index + stride] for index in range(0, len(pcm), stride)]


def iter_length_prefixed(blob: bytes) -> list[bytes]:
    """Split uint16-le Opus packets. Same framing the device uplinks."""
    frames: list[bytes] = []
    offset = 0
    while offset < len(blob):
        if offset + 2 > len(blob):
            raise ValueError("truncated opus header")
        size = blob[offset] | (blob[offset + 1] << 8)
        offset += 2
        if size == 0 or offset + size > len(blob):
            raise ValueError("truncated opus frame")
        frames.append(bytes(blob[offset : offset + size]))
        offset += size
    return frames


def length_prefixed_frame(packet: bytes) -> bytes:
    """One binary payload the device accepts when framing is length-prefixed."""
    if not packet or len(packet) > 1500:
        raise ValueError("opus packet must be 1..1500 bytes")
    return len(packet).to_bytes(2, "little") + packet


def device_host(device: str) -> tuple[str, int]:
    parsed = urllib.parse.urlparse(device)
    if parsed.scheme != "http" or not parsed.hostname:
        raise SystemExit("device must look like http://192.168.1.50:8080")
    return parsed.hostname, parsed.port or 8080


class SpeakClient:
    """Minimal WebSocket client. Sends are unfragmented."""

    def __init__(self, device: str, token: str, timeout: float = 10) -> None:
        self.device = device
        self.token = token
        self.timeout = timeout
        self.sock: Optional[socket.socket] = None
        self.buffer = b""

    def connect(self) -> None:
        host, port = device_host(self.device)
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        sock = socket.create_connection((host, port), self.timeout)
        sock.settimeout(self.timeout)
        request = (
            f"GET /api/speak HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n"
            f"Authorization: Bearer {self.token}\r\n"
            f"\r\n"
        )
        sock.sendall(request.encode("ascii"))
        status, headers, rest = _read_http_response(sock)
        self.buffer = rest
        if status != 101:
            sock.close()
            raise SystemExit(f"speak handshake failed: HTTP {status}")
        upgrade = headers.get("upgrade", "")
        if "websocket" not in upgrade.lower():
            sock.close()
            raise SystemExit("speak handshake missing Upgrade: websocket")
        self.sock = sock

    def start(self, **kwargs) -> dict:
        self._send(TEXT, json.dumps(start_message(**kwargs)).encode("utf-8"))
        reply = self._read_text()
        if not reply.get("ok"):
            raise SystemExit(f"speak rejected: {reply}")
        return reply

    def send_opus(self, packet: bytes) -> None:
        self._send(OPUS, packet)

    def send_pcm(self, pcm: bytes) -> None:
        self._send(OPUS, pcm)

    def end(self) -> dict:
        self._send(TEXT, b'{"event":"end"}')
        return self._read_text()

    def abort(self) -> None:
        self._send(TEXT, b'{"event":"abort"}')

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def _send(self, opcode: int, payload: bytes) -> None:
        if self.sock is None:
            raise RuntimeError("not connected")
        self.sock.sendall(encode_client_frame(opcode, payload, os.urandom(4)))

    def _read_text(self) -> dict:
        while True:
            frame = decode_server_frame(self.buffer)
            if frame is None:
                if self.sock is None:
                    raise SystemExit("speak socket closed")
                chunk = self.sock.recv(4096)
                if not chunk:
                    raise SystemExit("speak socket closed")
                self.buffer += chunk
                continue
            opcode, payload, self.buffer = frame
            if opcode == 0x8:
                raise SystemExit(f"speak closed: {payload!r}")
            if opcode == 0x9:
                self._send(0xA, payload)
                continue
            if opcode != TEXT:
                continue
            return json.loads(payload.decode("utf-8"))


def _read_http_response(sock: socket.socket) -> tuple[int, dict[str, str], bytes]:
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise SystemExit("speak handshake closed")
        data += chunk
        if len(data) > 16384:
            raise SystemExit("speak handshake too large")
    head, rest = data.split(b"\r\n\r\n", 1)
    lines = head.decode("iso-8859-1").split("\r\n")
    parts = lines[0].split()
    if len(parts) < 2 or not parts[1].isdigit():
        raise SystemExit(f"bad handshake status: {lines[0]}")
    headers: dict[str, str] = {}
    for line in lines[1:]:
        if ":" not in line:
            continue
        name, value = line.split(":", 1)
        headers[name.strip().lower()] = value.strip()
    return int(parts[1]), headers, rest


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, help="http://<device-ip>:8080")
    parser.add_argument("--token", required=True)
    parser.add_argument("--codec", choices=("opus", "pcm"), default="opus")
    parser.add_argument("--sample-rate", type=int, default=16000)
    parser.add_argument("--frame-ms", type=int, default=20)
    parser.add_argument("--framing", choices=("packet", "length-prefixed"), default="packet")
    parser.add_argument("--text", default="")
    parser.add_argument("--emotion", default="")
    parser.add_argument("--pcm", type=Path, help="raw little-endian mono s16le file")
    parser.add_argument("--opus", type=Path, help="raw Opus packet, or length-prefixed packets")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    if args.codec == "pcm" and args.opus:
        parser.error("--opus is for codec opus")
    if args.codec == "opus" and args.pcm:
        parser.error("--pcm is for codec pcm")
    if not args.dry_run and not args.pcm and not args.opus:
        parser.error("pass --pcm or --opus (or --dry-run)")

    message = start_message(
        codec=args.codec,
        sample_rate=args.sample_rate,
        frame_duration_ms=args.frame_ms,
        framing=args.framing,
        text=args.text or None,
        emotion=args.emotion or None,
    )
    chunks: list[bytes] = []
    if args.pcm:
        chunks = split_pcm(args.pcm.read_bytes(), args.sample_rate, args.frame_ms)
    elif args.opus:
        blob = args.opus.read_bytes()
        if args.framing == "length-prefixed":
            # The device reads uint16-le sizes inside the binary frame. Keep
            # the prefix on each chunk; do not send the bare Opus packet.
            chunks = [length_prefixed_frame(packet) for packet in iter_length_prefixed(blob)]
        else:
            chunks = [blob]

    if args.dry_run:
        print(json.dumps(message, ensure_ascii=False))
        print(f"chunks {len(chunks)}")
        return 0

    client = SpeakClient(args.device, args.token)
    try:
        client.connect()
        started = client.start(
            codec=args.codec,
            sample_rate=args.sample_rate,
            frame_duration_ms=args.frame_ms,
            framing=args.framing,
            text=args.text or None,
            emotion=args.emotion or None,
        )
        print(json.dumps(started, ensure_ascii=False))
        for chunk in chunks:
            if args.codec == "pcm":
                client.send_pcm(chunk)
            else:
                client.send_opus(chunk)
        print(json.dumps(client.end(), ensure_ascii=False))
    finally:
        client.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
