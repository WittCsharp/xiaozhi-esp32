# LAN Control API

A computer on the same Wi-Fi network can make the device play speech and show an emotion without the cloud push API. The Mac (or any other machine) synthesizes speech, hosts a mono Ogg Opus file, and sends the URL to the device. The device downloads that file and plays it with `Application::StartNotification`. There is no onboard TTS.

The cloud WebSocket and MQTT transports are unchanged. Notification playback still does not open a conversation and does not turn the microphone on. See [Asynchronous Voice Notifications](notify.md) for the audio rules.

This listener is **LAN-only** and has **no TLS**. The bearer token travels in cleartext. Do not port-forward it, and do not use it on a network you do not trust. The server binds after the station interface connects and stops before Wi-Fi provisioning AP mode, so it is not served on the setup hotspot.

`bread-compact-wifi-lcd` and every other `WifiBoard` pick this up from `main/boards/common`. No board pin changes are required. 4G-only boards do not start it. The Otto robot already listens on port 8080 for a different WebSocket server; do not enable both there.

## Enable

The API is off by default. `scripts/build.py` rebuilds `sdkconfig` from defaults plus the variant's `sdkconfig_append`, so a menuconfig-only change is dropped the next time that script runs.

For a local `bread-compact-wifi-lcd` build, add the options to that variant's `sdkconfig_append` in `main/boards/bread-compact-wifi-lcd/config.json` and do not commit a real token:

```json
"sdkconfig_append": [
    "CONFIG_LCD_ST7789_240X320=y",
    "CONFIG_LAN_CONTROL_API=y",
    "CONFIG_LAN_CONTROL_API_PORT=8080",
    "CONFIG_LAN_CONTROL_API_TOKEN=\"replace-with-a-long-random-secret\""
]
```

`CONFIG_LAN_CONTROL_API` selects the HTTP server WebSocket options the speak route needs (`HTTPD_WS_SUPPORT` and the pre-handshake callback). Leave those out of `sdkconfig_append`.

The committed `bread-compact-wifi-lcd` panel is the ST7789 line above. A GC9A01 240×240 circle uses `CONFIG_LCD_GC9A01_240X240=y` **instead of** that ST7789 line. Do not enable both. This API does not change the default panel.

```bash
python3 scripts/build.py bread-compact-wifi-lcd
```

Menuconfig path, if you build with `idf.py` directly: **Xiaozhi Assistant → LAN Control API**.

NVS namespace `lan_api`, key `token`, overrides the compiled token. The firmware reads it on every request. Both names are a persistent setting; an empty NVS value falls back to the Kconfig token. A token longer than 128 characters is ignored. If neither source has a token, the port still opens and every request returns `503` with `token_not_configured`.

On boot, after Wi-Fi connects, the log line looks like:

```text
I (12345) LanControl: LAN control API at http://192.168.1.50:8080
```

## Requests

Send `Authorization: Bearer <token>` on every route, including health. POST bodies use `Content-Type: application/json` and must be at most 4 KB.

Replace the addresses below with the device and the machine that hosts the audio.

```bash
export XIAOZHI_LAN_TOKEN='replace-with-a-long-random-secret'
export DEVICE=http://192.168.1.50:8080

curl -sS "$DEVICE/api/health" \
  -H "Authorization: Bearer $XIAOZHI_LAN_TOKEN"

curl -sS "$DEVICE/api/alert" \
  -H "Authorization: Bearer $XIAOZHI_LAN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"status":"Hello","message":"From the Mac","emotion":"happy"}'

curl -sS "$DEVICE/api/notify" \
  -H "Authorization: Bearer $XIAOZHI_LAN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"audio_url":"http://192.168.1.20:8090/notify.ogg","subtitles":[{"start_ms":0,"text":"Hello from the Mac"}]}'
```

`GET /api/health` returns `200`:

```json
{"ok":true,"state":"idle","ip":"192.168.1.50","port":8080,"body_mode":false,"bridge":"missing"}
```

`body_mode` is whether the wake word and BOOT record locally instead of opening cloud chat. Saying the compiled wake word starts that listen; BOOT is an optional start/stop. `bridge` is `configured` when a Mac bridge URL is stored, otherwise `missing`. See [Bot as brain](bot-as-brain.md).

`POST /api/alert` calls `Application::Alert(status, message, emotion)` on the main loop and returns `200`. It updates the status line, emotion, and subtitle. It does not play the cloud alert vibration. `status`, `message`, and `emotion` are required, non-empty strings. Emotion names are the same ones the cloud sends, such as `neutral`, `happy`, and `sad`. Unknown names are still forwarded to the display.

`POST /api/notify` calls `Application::EnqueueNotification`, which runs `StartNotification` on the main loop. The device must be `idle`. The response is `202`:

```json
{"ok":true,"queued":true}
```

Any other state returns `409`:

```json
{"ok":false,"error":"busy","state":"speaking"}
```

`audio_url` is required. `subtitles` is an optional array of `{ "start_ms": <number>, "text": "<string>" }`, the same shape as the cloud `notify` message. A second request that arrives before the main loop has left `idle` can also be queued; only the first one plays.

### Audio file

`audio_url` must be `http://` or `https://` (lowercase scheme) and must return `2xx` with one mono Ogg Opus stream. The device does not follow redirects. Packet durations from 5 ms through 120 ms are accepted. Stereo, a non-Ogg body, and an incomplete stream are rejected and the device returns to `idle`.

Host the file on the computer, not on the device. Use the computer's LAN address in `audio_url`. `localhost` is the device itself.

```bash
say -o /tmp/notify.aiff "Hello from the Mac"
ffmpeg -y -i /tmp/notify.aiff -ar 16000 -ac 1 -c:a libopus -b:a 24k \
  -frame_duration 20 -vn /tmp/notify.ogg
python3 -m http.server 8090 --bind 0.0.0.0 --directory /tmp
```

Then `audio_url` is `http://<mac-lan-ip>:8090/notify.ogg`. macOS may prompt to allow incoming connections to Python.

`scripts/lan_control/mac_speak.py` does those steps and posts the URL. It uses `say` and `ffmpeg`.

```bash
python3 scripts/lan_control/mac_speak.py \
  --device "$DEVICE" \
  --token "$XIAOZHI_LAN_TOKEN" \
  --text "Hello from the Mac" \
  --emotion happy
```

`--emotion` posts `/api/alert` first, then `/api/notify` when `--text` or `--ogg` is set. `--ogg` skips TTS and serves an existing file. The script keeps the file server up until the device returns to `idle` or the wait expires.

`mac_speak.py` waits until `say` and `ffmpeg` have written the whole file, then serves it with `Content-Length`. The device side of `/api/notify` does **not** wait for that file to finish downloading: `NotifyPlayer` demuxes Ogg and queues Opus as the HTTP body arrives (see [notify.md](notify.md)). The gap is on the computer. A bridge that only calls `mac_speak.py` cannot start the speaker until TTS has produced a complete Ogg.

## Streaming speak

`GET /api/speak` upgrades to a WebSocket when body mode is on. The Mac pushes Opus or PCM as it is synthesized. The device enters `speaking` and plays each frame before the rest of the utterance exists. It does not open the cloud audio channel, send `start-listening`, or run cloud Q&A. `/api/notify` stays the fallback for a finished Ogg URL.

The listener is still LAN-only and cleartext. Send `Authorization: Bearer <token>` on the handshake. The pre-handshake check rejects a bad token with HTTP 401/503 and does not switch protocols.

`esp_http_server` does not decode `Transfer-Encoding: chunked` request bodies, so a chunked POST cannot start playback early. Use WebSocket frames. Each frame is unfragmented (FIN set).

First frame, text JSON:

```json
{"event":"start","codec":"opus","sample_rate":16000,"channels":1,"frame_duration_ms":20,"framing":"packet","text":"Hello","emotion":"happy"}
```

| Field | Required | Meaning |
| --- | --- | --- |
| `event` | yes | `start`, then later `end` or `abort` |
| `codec` | start | `opus` (default) or `pcm` |
| `sample_rate` | no | 8000, 12000, 16000 (default), 24000, or 48000 |
| `channels` | no | `1` only |
| `frame_duration_ms` | opus | Hint. Each Opus packet's TOC duration is what is played. 5–120 ms, not 2.5 ms |
| `framing` | opus | `packet` (one raw Opus packet per binary frame) or `length-prefixed` (same `uint16` little-endian sizes as the uplink clip) |
| `text` | no | Assistant line, at most 200 bytes. Shown when playback is accepted |
| `emotion` | no | Same names as `/api/alert` |

PCM binary frames are little-endian mono `s16le` at `sample_rate`. There is no length prefix. Send about 20–60 ms per frame so the first chunk is small. The device resamples to the codec rate.

The device replies with a text frame:

```json
{"ok":true,"event":"playing","playback_id":1}
```

Binary frames after that are played immediately. There is no popup chime on this path. `{"event":"end"}` means no more audio; the device answers `{"ok":true,"event":"ended"}` and stays in `speaking` until the queued audio drains, then returns to `idle`. Closing the socket after `playing` is the same as `end`: the tail still plays. Poll `/api/health`. `{"event":"abort"}`, a bad frame, or a wake word drops the tail. A wake word then starts a local listen.

Busy, body mode off, and a bad frame are text errors, then the socket closes:

```json
{"ok":false,"error":"busy","state":"listening"}
```

| `error` | When |
| --- | --- |
| `body_mode_off` | `/api/mode` has not turned body mode on |
| `busy` | Not idle, already speaking, capturing, or a notification is playing |
| `timeout` | The main loop did not accept the stream |
| `aborted` | Wake, BOOT, or another stop won the race |
| `invalid_speak` | JSON, codec, rate, or audio frame rejected |

A second stream is refused while one is active. The decode queue holds about 1.2 s of Opus, and the speak task holds at most 16 KB waiting to be queued, so a fast sender blocks instead of allocating the whole utterance.

`scripts/lan_control/stream_speak.py` is the client. Push a raw file:

```bash
python3 scripts/lan_control/stream_speak.py \
  --device "$DEVICE" \
  --token "$XIAOZHI_LAN_TOKEN" \
  --codec pcm \
  --sample-rate 24000 \
  --frame-ms 20 \
  --pcm reply.s16le \
  --text "Hello" \
  --emotion happy
```

```python
# From a bridge, with scripts/lan_control on PYTHONPATH, as TTS yields PCM:
import sys
sys.path.append("scripts/lan_control")
from stream_speak import SpeakClient

client = SpeakClient(device, token)
client.connect()
client.start(codec="pcm", sample_rate=24000, text=reply_text, emotion="happy")
for chunk in tts_pcm_chunks():  # s16le mono; send each chunk when it exists
    client.send_pcm(chunk)
client.end()
client.close()
```

Opus is the smaller path. One binary frame, one packet, `framing` left at `packet`. `frame_duration_ms` should match the encoder (20 ms for the `ffmpeg` line above). The TOC on the packet overrides that hint when it is a duration the firmware decoder accepts.

## Body mode

`POST /api/mode` and `POST /api/bridge` use the same bearer token. They store settings in NVS namespace `lan_api`. Cloud chat is unchanged while body mode is off.

```bash
curl -sS "$DEVICE/api/mode" \
  -H "Authorization: Bearer $XIAOZHI_LAN_TOKEN"

curl -sS -X POST "$DEVICE/api/mode" \
  -H "Authorization: Bearer $XIAOZHI_LAN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"body":true}'

curl -sS -X POST "$DEVICE/api/bridge" \
  -H "Authorization: Bearer $XIAOZHI_LAN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"url":"http://192.168.1.20:9100"}'
```

`body` is a JSON boolean. The response is `200`:

```json
{"ok":true,"body_mode":true,"bridge":"configured"}
```

`url` is `http://` or `https://` with a host, at most 480 characters. An empty string clears it. `token` is optional. Omit it to leave the stored bridge token alone; send `""` to clear it. The token is not returned. `GET /api/bridge` returns the URL and `token_set`.

NVS keys, all in `lan_api`: `body_mode`, `bot_bridge_url`, and `bridge_token`. The token key is not `bot_bridge_token` because an NVS key is at most 15 characters. When `body_mode` has never been written, Kconfig `BODY_MODE_DEFAULT` applies (off unless you turn it on).

Turning body mode off while the device is in a local listen stops that listen. The next BOOT uses cloud chat again.

## Servo stub

`POST /api/servo` is reserved so a later pan/tilt driver can share this port. It checks `pan` and `tilt` as whole degrees from -90 to 90 and returns `200` without driving a pin:

```bash
curl -sS "$DEVICE/api/servo" \
  -H "Authorization: Bearer $XIAOZHI_LAN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"pan":10,"tilt":-5}'
```

```json
{"ok":true,"stub":true,"pan":10,"tilt":-5,"pan_gpio":8,"tilt_gpio":9}
```

GPIO8 is the intended pan pin and GPIO9 is the intended tilt pin. They are not claimed, because many boards already use them.

## Errors

| HTTP | `error` | When |
| --- | --- | --- |
| 401 | `unauthorized` | Missing or wrong bearer token |
| 400 | `invalid_json`, `invalid_audio_url`, `invalid_subtitles`, `invalid_alert`, `invalid_servo`, `invalid_mode`, `invalid_bridge` | Body rejected |
| 409 | `busy` | Notify while the device is not idle |
| 413 | `payload_too_large` | Body over 4 KB |
| 415 | `unsupported_media_type` | POST without `application/json` |
| 503 | `token_not_configured` | No Kconfig token and no NVS token |

Playback is best effort. `202` means the main loop was asked to start, not that the file played. Poll `/api/health` and watch `state` move through `notifying` back to `idle`. A bad file, a redirect, or a network error ends playback and returns the device to `idle`.
