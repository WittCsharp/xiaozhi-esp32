"""Host checks for the LAN control API helpers and wiring."""

import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def load_mac_speak():
    path = ROOT / "scripts" / "lan_control" / "mac_speak.py"
    spec = importlib.util.spec_from_file_location("mac_speak", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class LanControlUtilTests(unittest.TestCase):
    def test_url_token_and_angle_checks(self):
        source = ROOT / "scripts" / "tests" / "lan_control_util_test.cc"
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "lan_control_util_test"
            compiled = subprocess.run(
                [
                    "g++",
                    "-std=c++23",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{ROOT / 'main' / 'boards' / 'common'}",
                    str(source),
                    "-o",
                    str(binary),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            if compiled.returncode != 0:
                self.fail(compiled.stderr)
            ran = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True, check=False)
            if ran.returncode != 0:
                self.fail(ran.stderr or ran.stdout)


class LanControlScriptTests(unittest.TestCase):
    def test_ffmpeg_command_is_mono_ogg_opus(self):
        speak = load_mac_speak()
        command = speak.ffmpeg_command(Path("in.aiff"), Path("out.ogg"))
        self.assertEqual(command[0], "ffmpeg")
        self.assertIn("-ac", command)
        self.assertEqual(command[command.index("-ac") + 1], "1")
        self.assertEqual(command[command.index("-c:a") + 1], "libopus")
        self.assertEqual(command[command.index("-frame_duration") + 1], "20")
        self.assertTrue(str(command[-1]).endswith(".ogg"))

    def test_payloads_match_the_device_contract(self):
        speak = load_mac_speak()
        notify = speak.notify_payload("http://192.168.1.20:8090/notify.ogg", "Hello")
        self.assertEqual(
            notify,
            {
                "audio_url": "http://192.168.1.20:8090/notify.ogg",
                "subtitles": [{"start_ms": 0, "text": "Hello"}],
            },
        )
        self.assertEqual(
            speak.notify_payload("http://192.168.1.20/a.ogg", None),
            {"audio_url": "http://192.168.1.20/a.ogg"},
        )
        self.assertEqual(
            speak.alert_payload("LAN", "Hello", "happy"),
            {"status": "LAN", "message": "Hello", "emotion": "happy"},
        )

    def test_dry_run_prints_notify_without_calling_the_device(self):
        script = ROOT / "scripts" / "lan_control" / "mac_speak.py"
        ran = subprocess.run(
            [
                "python3",
                str(script),
                "--device",
                "http://192.168.1.50:8080",
                "--token",
                "secret",
                "--text",
                "Hello",
                "--emotion",
                "happy",
                "--dry-run",
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(ran.returncode, 0, ran.stderr)
        self.assertNotIn("secret", ran.stdout)
        self.assertIn("-ac", ran.stdout)
        self.assertIn("libopus", ran.stdout)
        payloads = [json.loads(line) for line in ran.stdout.splitlines() if line.startswith("{")]
        self.assertEqual(payloads[0]["emotion"], "happy")
        self.assertEqual(payloads[1]["audio_url"], "http://192.0.2.10:8090/notify.ogg")
        self.assertEqual(payloads[1]["subtitles"][0]["text"], "Hello")


class LanControlWiringTests(unittest.TestCase):
    def test_routes_use_existing_application_apis(self):
        server = (ROOT / "main" / "boards" / "common" / "lan_control_server.cc").read_text(encoding="utf-8")
        for route in (
            "/api/health",
            "/api/notify",
            "/api/alert",
            "/api/servo",
            "/api/mode",
            "/api/bridge",
        ):
            self.assertIn(route, server)
        self.assertIn("body_mode", server)
        self.assertIn("invalid_mode", server)
        self.assertIn("invalid_bridge", server)
        self.assertIn("EnqueueNotification", server)
        self.assertIn(".Alert(", server)
        self.assertNotIn("driver/gpio.h", server)

        app = (ROOT / "main" / "application.cc").read_text(encoding="utf-8")
        self.assertIn("void Application::EnqueueNotification", app)
        self.assertIn("StartNotification(std::move(audio_url), std::move(subtitles))", app)

        wifi = (ROOT / "main" / "boards" / "common" / "wifi_board.cc").read_text(encoding="utf-8")
        self.assertIn("LanControlServer::GetInstance().Start()", wifi)
        self.assertIn("LanControlServer::GetInstance().Stop()", wifi)

        kconfig = (ROOT / "main" / "Kconfig.projbuild").read_text(encoding="utf-8")
        self.assertIn("config LAN_CONTROL_API", kconfig)
        self.assertIn('default ""', kconfig)
        self.assertIn("lan_api", kconfig)
        self.assertIn("config BODY_MODE_DEFAULT", kconfig)

    def test_body_mode_boot_skips_cloud_open(self):
        app = (ROOT / "main" / "application.cc").read_text(encoding="utf-8")
        toggle = _definition(app, "void Application::HandleToggleChatEvent()")
        self.assertLess(toggle.index("BeginBodyListen"), toggle.index("ContinueOpenAudioChannel"))
        begin = _definition(app, "void Application::BeginBodyListen(const char* source)")
        self.assertNotIn("OpenAudioChannel", begin)
        self.assertIn("cloud chat skipped", begin)
        reopen = _definition(app, "void Application::ContinueOpenAudioChannel(ListeningMode mode)")
        self.assertLess(reopen.index("body_bridge::Enabled()"), reopen.index("protocol_->OpenAudioChannel"))
        wake_continue = _definition(
            app, "void Application::ContinueWakeWordInvoke(const std::string& wake_word)"
        )
        self.assertLess(
            wake_continue.index("body_bridge::Enabled()"),
            wake_continue.index("protocol_->OpenAudioChannel"),
        )
        wake = _definition(app, "void Application::HandleWakeWordDetectedEvent()")
        self.assertLess(wake.index("HandleBodyWakeWord"), wake.index("BeginWakeWordInvoke"))
        send = app.split("if (bits & MAIN_EVENT_SEND_AUDIO)", 1)[1].split(
            "MAIN_EVENT_WAKE_WORD_DETECTED", 1
        )[0]
        self.assertLess(send.index("RouteBodyUplink"), send.index("SendAudio"))
        uplink = (ROOT / "main" / "boards" / "common" / "body_bridge.cc").read_text(encoding="utf-8")
        self.assertIn('kBodyModeKey = "body_mode"', uplink)
        self.assertIn('kBridgeUrlKey = "bot_bridge_url"', uplink)
        self.assertIn('kBridgeTokenKey = "bridge_token"', uplink)
        cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("body_bridge.cc", cmake)


def _definition(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


if __name__ == "__main__":
    unittest.main()
