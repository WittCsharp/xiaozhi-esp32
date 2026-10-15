"""Host checks for the Mac body-mode bridge stub."""

import json
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

import importlib.util


ROOT = Path(__file__).resolve().parents[2]


def load_bridge():
    path = ROOT / "scripts" / "lan_control" / "body_bridge.py"
    spec = importlib.util.spec_from_file_location("body_bridge", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BodyBridgeTests(unittest.TestCase):
    def test_opus_frames_round_trip_the_device_framing(self):
        bridge = load_bridge()
        blob = bytes([0x02, 0x00, ord("a"), ord("b"), 0x03, 0x00, ord("x"), ord("y"), ord("z")])
        self.assertEqual(bridge.parse_opus_frames(blob), [b"ab", b"xyz"])
        self.assertEqual(bridge.parse_opus_frames(b""), [])
        with self.assertRaises(ValueError):
            bridge.parse_opus_frames(b"\x02\x00\x61")

    def test_speak_command_targets_mac_speak(self):
        bridge = load_bridge()
        command = bridge.speak_command("http://192.168.1.50:8080", "secret", "Hello", "thinking")
        self.assertEqual(command[1], str(bridge.MAC_SPEAK))
        self.assertEqual(command[command.index("--device") + 1], "http://192.168.1.50:8080")
        self.assertEqual(command[command.index("--token") + 1], "secret")
        self.assertEqual(command[command.index("--text") + 1], "Hello")
        self.assertEqual(command[command.index("--emotion") + 1], "thinking")

    def test_handler_logs_event_and_counts_audio_without_speaking(self):
        bridge = load_bridge()
        config = bridge.BridgeConfig("", "", None)
        status, body = bridge.handle_uplink(
            "/body/event",
            {},
            json.dumps({"event": "listen_start", "source": "button", "device_id": "mac"}).encode(),
            config,
        )
        self.assertEqual(status, 200)
        self.assertTrue(body["ok"])

        blob = bytes([0x01, 0x00, 0x11])
        status, body = bridge.handle_uplink(
            "/body/audio",
            {"x-source": "button", "x-sample-rate": "16000", "x-frame-duration-ms": "60"},
            blob,
            config,
        )
        self.assertEqual((status, body["frames"], body["bytes"]), (200, 1, 3))
        self.assertEqual(config.clips, 1)

        status, body = bridge.handle_uplink("/nope", {}, b"", config)
        self.assertEqual(status, 404)

    def test_http_server_accepts_an_event(self):
        bridge = load_bridge()
        config = bridge.BridgeConfig("", "", None)
        server = bridge.ThreadingHTTPServer(("127.0.0.1", 0), bridge.make_handler(config))
        port = server.server_address[1]
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        thread.start()
        self.addCleanup(server.shutdown)
        self.addCleanup(server.server_close)
        self.addCleanup(thread.join, 2)
        payload = json.dumps({"event": "listen_stop", "source": "wake"}).encode()
        request = urllib.request.Request(
            f"http://127.0.0.1:{port}/body/event",
            data=payload,
            method="POST",
        )
        request.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(request, timeout=2) as response:
            self.assertEqual(response.status, 200)
            self.assertTrue(json.loads(response.read().decode())["ok"])
        with self.assertRaises(urllib.error.HTTPError) as caught:
            urllib.request.urlopen(f"http://127.0.0.1:{port}/missing", timeout=2)
        self.assertEqual(caught.exception.code, 404)


if __name__ == "__main__":
    unittest.main()
