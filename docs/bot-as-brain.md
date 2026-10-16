# Bot as brain, Xiaozhi as body

Body mode makes the device a microphone, speaker, and face. A computer on the same Wi-Fi is the brain. Cloud WebSocket chat stays available when body mode is off.

Downlink is the [LAN control API](lan-control.md). `POST /api/notify` still plays a finished Ogg file. While body mode is on, `GET /api/speak` is a WebSocket the Mac can push Opus or PCM into before that file exists. The board has no onboard TTS. `scripts/lan_control/stream_speak.py` is the streaming client. `scripts/lan_control/mac_speak.py` remains the notify fallback.

```text
wake word (BOOT starts a clip from standby, or ends an open conversation)
    -> chime, then the device records Opus locally (no cloud socket)
    -> POST {bridge}/body/event and {bridge}/body/audio
    -> Mac bridge (ASR and the Bot are still TODO)
    -> stream_speak.py -> WebSocket /api/speak
       or mac_speak.py -> POST /api/alert and POST /api/notify
    -> playback ends and the mic opens again, no wake word
    -> idle window expires, or BOOT, and the next start needs the wake word
```

The spoken wake word is the normal way to start a conversation. It calls `BeginBodyListen("wake")` and does not open cloud chat. The mic stays closed while the wake chime plays, so that sound is not treated as the utterance. After each reply the device listens again without that chime. BOOT from standby still starts a clip. BOOT during a conversation ends it. See [Conversation](lan-control.md#conversation).

Wake-word language defaults are unchanged. An en-US build still listens for "Hi ESP" (`wn9_hiesp` on ESP32-S3). RoboEyes and other face drawing are unchanged. The feature is compiled only when `CONFIG_LAN_CONTROL_API` is on, so boards built without that option keep the previous BOOT path.

## Flash once

Body mode is a runtime flag, but the uplink code has to be in the firmware. Build with the LAN control API enabled, as in [lan-control.md](lan-control.md). Do not commit a real token. `BODY_MODE_DEFAULT` can stay off; turn the mode on over HTTP after boot.

## Enable

On the Mac, note the LAN address (`ipconfig getifaddr en0` on Wi-Fi). `localhost` on the device is the device itself, so the bridge URL must be the Mac's address.

Terminal 1, on the Mac:

```bash
python3 scripts/lan_control/body_bridge.py --port 9100 \
  --device "$DEVICE" \
  --token "$XIAOZHI_LAN_TOKEN"
```

`$DEVICE` is `http://<device-ip>:8080`. macOS may ask to allow incoming connections.

Terminal 2:

```bash
export XIAOZHI_LAN_TOKEN='replace-with-a-long-random-secret'
export DEVICE=http://192.168.1.50:8080

curl -sS -X POST "$DEVICE/api/mode" \
  -H "Authorization: Bearer $XIAOZHI_LAN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"body":true}'

curl -sS -X POST "$DEVICE/api/bridge" \
  -H "Authorization: Bearer $XIAOZHI_LAN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"url":"http://192.168.1.20:9100"}'

curl -sS "$DEVICE/api/health" \
  -H "Authorization: Bearer $XIAOZHI_LAN_TOKEN"
```

Health should include `"body_mode":true` and `"bridge":"configured"`. Optional `"token"` on `/api/bridge` is a separate bearer the device sends to the Mac. The stub does not require it. The LAN API token is not reused unless you copy it into that field.

To go back to cloud chat:

```bash
curl -sS -X POST "$DEVICE/api/mode" \
  -H "Authorization: Bearer $XIAOZHI_LAN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"body":false}'
```

## Wake word on the flash

This change does not switch the board to Chinese. `scripts/build.py` picks the model from the language: en-US and every language other than zh-CN/zh-TW use "Hi ESP". zh-CN and zh-TW use 「你好小智」. Board `config.json` must not pin the model.

Say the phrase that was compiled in. A default en-US build listens for **Hi ESP**. To bake 「你好小智」 and keep the English UI:

```bash
python3 scripts/build.py bread-compact-wifi-lcd --wake-word nihaoxiaozhi
```

`--language zh-CN` changes both the UI strings and the wake word. Keep `CONFIG_LAN_CONTROL_API` in the variant `sdkconfig_append`, as in [lan-control.md](lan-control.md). The committed `bread-compact-wifi-lcd` variant still selects the ST7789 panel. A GC9A01 circle needs **Xiaozhi Assistant → LCD Type → GC9A01 240*240 Circle** (`CONFIG_LCD_GC9A01_240X240=y`) in place of that ST7789 line. This patch does not change that default.

## Listen test

1. Leave the device idle. The face should say standby, not connecting.
2. Say the compiled wake word. The face shows listening and plays the short chime. It must not open the cloud conversation. The bridge logs `event listen_start` with `"source":"wake"`.
3. After the chime, speak a short sentence. The first time local VAD goes quiet the bridge gets `speech_pause`. That event means the clip is still open; do not speak a reply yet. About 0.6 s of continued silence, or eight seconds, ends the clip. A second BOOT press also ends it.
4. The bridge logs the Opus byte count and frame count, then prints a `mac_speak.py` command. The device shows emotion `thinking` and stays idle so a notification can play. Health shows `"conversation":true`.
5. Run that command (or start the bridge with `--reply "Hello from the bridge"`). The device should play it through `/api/notify` and show the emotion through `/api/alert`. When playback finishes, the face returns to listening without the wake chime. Speak again. The bridge logs another clip with `"source":"followup"`.
6. Stay quiet after a reply. After the idle window (20 s unless `CONFIG_BODY_CONVERSATION_IDLE_MS` is changed), the bridge logs `listen_stop` with `"reason":"session"` and the face returns to standby. The next sentence needs the wake word. BOOT during the conversation returns to standby immediately.

BOOT from standby does the same local capture without the chime: press once to start, press again to stop. Hold-to-talk boards use that button path too: press starts the clip, release uploads it. A release ends the conversation, so the next press starts a new one.

If the bridge URL is missing, the wake word and BOOT still do not open cloud chat. The screen says to set the bridge URL.

## What the device sends

`POST {bridge}/body/event` with `Content-Type: application/json`:

```json
{"event":"listen_start","source":"button","device_id":"aa:bb:cc:dd:ee:ff","ip":"192.168.1.50"}
```

`event` is `listen_start`, `speech_pause`, `utterance`, or `listen_stop`. `source` is `button`, `wake`, or `followup`. `speech_pause` is sent once, the first time VAD goes quiet after speech, and includes the same counters. The bytes are not uploaded yet. `utterance` and `listen_stop` add `reason` (`button`, `silence`, `timeout`, `full`, or `session`), `frames`, `bytes`, `sample_rate`, and `frame_duration_ms`. `reason` `silence` is the end of the utterance, about 0.6 s after VAD went quiet. `reason` `session` is the follow-up mic closing with no speech. That event is `listen_stop` only.

`POST {bridge}/body/audio` with `Content-Type: application/octet-stream` is the clip. Each Opus packet is a little-endian `uint16` length followed by that many raw Opus bytes (not an Ogg page). Headers:

| Header | Example |
| --- | --- |
| `X-Audio-Codec` | `opus` |
| `X-Sample-Rate` | `16000` |
| `X-Channels` | `1` |
| `X-Frame-Duration-Ms` | `60` |
| `X-Frame-Count` | `12` |
| `X-Source` | `button` |
| `Device-Id` | MAC address |

The clip stops at 32 KB, eight seconds, about 0.6 s of silence after speech, or another BOOT press. The conversation stays open after a silence, full, or 8 s stop that contained speech. The main loop does not wait on the HTTP upload. A notify reply still has to find the device `idle`; poll `/api/health` if notify returns `409`. While it is waiting, `conversation` is true. A streaming reply uses `/api/speak` while the device is idle and can start on the first audio chunk. When playback finishes, the device listens again. See [Streaming speak](lan-control.md#streaming-speak) and [Conversation](lan-control.md#conversation).

`scripts/lan_control/body_bridge.py` only logs the clip and can call `mac_speak.py`. Wiring the transcript into the Grok Bot is left as a TODO in that script.
