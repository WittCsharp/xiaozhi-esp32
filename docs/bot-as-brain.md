# Bot as brain, Xiaozhi as body

Body mode makes the device a microphone, speaker, and face. A computer on the same Wi-Fi is the brain. Cloud WebSocket chat stays available when body mode is off.

Downlink is the existing [LAN control API](lan-control.md): `POST /api/alert` and `POST /api/notify`. This page is the uplink. The board has no onboard TTS. The Mac synthesizes speech with `scripts/lan_control/mac_speak.py`.

```text
BOOT or wake
    -> device records Opus locally (no cloud socket)
    -> POST {bridge}/body/event and {bridge}/body/audio
    -> Mac bridge (ASR and the Bot are still TODO)
    -> mac_speak.py -> POST /api/alert and POST /api/notify
```

Wake-word language defaults are unchanged. RoboEyes and other face drawing are unchanged. The feature is compiled only when `CONFIG_LAN_CONTROL_API` is on, so boards built without that option keep the previous BOOT path.

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

## BOOT test

1. Leave the device idle. The face should say standby, not connecting.
2. Press BOOT once. The device shows listening. It must not open the cloud conversation. The bridge logs `event listen_start`.
3. Speak a short sentence, then press BOOT again. Silence for about one second after speech, or eight seconds, also ends the clip.
4. The bridge logs the Opus byte count and frame count, then prints a `mac_speak.py` command. The device shows emotion `thinking` and returns to idle so a notification can play.
5. Run that command (or start the bridge with `--reply "Hello from the bridge"`). The device should play it through `/api/notify` and show the emotion through `/api/alert`.

Wake does the same local capture when body mode is on, so a wake word does not open cloud chat either. Hold-to-talk boards use the same path: press starts the clip, release uploads it.

If the bridge URL is missing, BOOT still does not open cloud chat. The screen says to set the bridge URL.

## What the device sends

`POST {bridge}/body/event` with `Content-Type: application/json`:

```json
{"event":"listen_start","source":"button","device_id":"aa:bb:cc:dd:ee:ff","ip":"192.168.1.50"}
```

`event` is `listen_start`, `utterance`, or `listen_stop`. `source` is `button` or `wake`. `utterance` and `listen_stop` add `reason` (`button`, `silence`, `timeout`, or `full`), `frames`, `bytes`, `sample_rate`, and `frame_duration_ms`.

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

The clip stops at 32 KB, eight seconds, a second of silence after speech, or another BOOT press. The main loop does not wait on the HTTP upload. A reply still has to find the device `idle`; poll `/api/health` if notify returns `409`.

`scripts/lan_control/body_bridge.py` only logs the clip and can call `mac_speak.py`. Wiring the transcript into the Grok Bot is left as a TODO in that script.
