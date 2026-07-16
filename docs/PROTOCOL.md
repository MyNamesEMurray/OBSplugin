# LensLink — Wire Protocol (version 1)

The iOS app **listens** on TCP port **9979** on the device; the OBS plugin
**dials** it — over the LAN (the phone's IP, shown in the app) or through
usbmuxd (USB cable). Once connected, the app immediately sends HELLO and
the packet stream begins.

All multi-byte integers are **big-endian** (network byte order).

## Packet framing

Every packet starts with a fixed 20-byte header, immediately followed by
`payload_size` bytes of payload:

| Offset | Size | Field          | Notes                                    |
|--------|------|----------------|------------------------------------------|
| 0      | 4    | magic          | ASCII `OBSC` (0x4F 0x42 0x53 0x43)       |
| 4      | 1    | version        | `1`                                      |
| 5      | 1    | type           | see packet types below                   |
| 6      | 2    | flags          | bit 0 (`0x0001`) = keyframe (video only) |
| 8      | 8    | pts            | presentation timestamp, **nanoseconds**  |
| 16     | 4    | payload_size   | max 16 MiB (`16 * 1024 * 1024`)          |

A receiver that sees a bad magic, an unknown version, or an oversized
`payload_size` must drop the connection.

## Packet types

### 1 — HELLO
Sent once by the client right after the TCP connection is established.
Payload: UTF-8 JSON, e.g.

```json
{ "name": "Emma's iPhone", "app": "LensLink", "protocol": 1, "kind": "camera" }
```

`kind` is `"camera"` (the app) or `"screen"` (the screen-mirror broadcast
extension); it lets the plugin label the source and, for `screen`, expect
system audio (packet type 10) instead of camera controls. Absent = camera.

An optional `"standby": true` means the app is reachable but the camera
isn't running yet (the app is open and idle with its **Remote start from
OBS** option on). No video follows until the plugin sends a
`start_stream` control command (type 7). The app re-sends HELLO (without
`standby`) when the stream starts; the plugin also treats VIDEO_CONFIG as
leaving standby, so either signal suffices. Absent = a live stream
follows as usual.

### 2 — VIDEO_CONFIG
Sent after HELLO and again whenever the capture format changes.
Payload: UTF-8 JSON, e.g.

```json
{ "codec": "h264", "width": 1280, "height": 720, "fps": 30, "kind": "camera" }
```

`codec` is `"h264"` or `"hevc"` and selects the plugin's decoder; a codec
change mid-stream resets the decoder (the next keyframe re-initializes it).
`kind` mirrors the HELLO field. Dimensions/fps are informational; the
authoritative values come from the bitstream parameter sets.

### 3 — VIDEO
Payload: one H.264 or HEVC **access unit in Annex B format** (start-code
delimited NAL units), per the codec announced in VIDEO_CONFIG. Keyframe
packets must set the keyframe flag and must be self-contained: parameter
sets (SPS/PPS for H.264, VPS/SPS/PPS for HEVC) are prepended before the
IDR/IRAP slice so a decoder can join mid-stream.

`pts` is the capture presentation timestamp in nanoseconds. It only needs to
be monotonic; OBS re-bases async timestamps itself.

### 4 — PING
Optional keep-alive, empty payload, sent by the app every ~2 s.
The plugin ignores it.

### 5 — TIMESYNC_REQ (plugin → app)
Empty payload; `pts` = the plugin's monotonic clock (t1, nanoseconds).
Sent about once a second while connected.

### 6 — TIMESYNC_RESP (app → plugin)
Sent immediately upon receiving a TIMESYNC_REQ. `pts` = the device's
host clock (t2) — the **same clock domain as video frame timestamps** —
and the payload is the echoed 8-byte big-endian t1.

The plugin computes, NTP-style, with t3 = its clock at receipt:
`rtt = t3 - t1`, `offset = t2 - (t1 + t3)/2` (phone clock minus plugin
clock, error ≤ rtt/2). Per-frame capture→decode latency is then
`now - (frame_pts - offset)`, logged and shown in the source status.

### 7 — CONTROL (plugin → app)
Camera remote control. Payload: UTF-8 JSON, one command per packet:

```json
{ "cmd": "zoom", "value": 2.0 }
{ "cmd": "exposure_bias", "value": -0.5 }
{ "cmd": "focus", "mode": "auto" }
{ "cmd": "focus", "mode": "locked", "lensPosition": 0.42 }
{ "cmd": "flashlight", "on": true }
{ "cmd": "flip" }
{ "cmd": "white_balance", "mode": "locked", "temperature": 5600 }
{ "cmd": "white_balance", "mode": "auto" }
{ "cmd": "exposure", "mode": "manual", "iso": 400, "shutterSeconds": 0.004 }
{ "cmd": "exposure", "mode": "auto" }
{ "cmd": "start_stream" }
{ "cmd": "stop_stream" }
{ "cmd": "set_format", "resolution": "1080p", "fps": 60, "codec": "hevc" }
{ "cmd": "mic", "id": "builtin:2" }
```

`set_format` switches the capture format mid-stream; any subset of its
fields may be present. The app validates the combination against the
active lens (the STATE snapshot advertises the valid choices in
`resolutions` / `frameRates` / `codecs`, plus the current `resolution` /
`fps` / `codec`) and ignores unsupported requests. A format change flows
through the normal live-reconfigure path: new VIDEO_CONFIG, fresh
keyframe, decoder reset on a codec change.

`mic` selects which microphone feeds the phone-mic capture, hot-switchable
mid-stream. Ids come from the STATE snapshot's `mics` list: `"auto"`
(system routing), `"builtin:<dataSourceID>"` (a physical phone mic —
Bottom/Front/Back), or `"port:<uid>"` (an external input). The app
validates the id against the live list and ignores stale ones (e.g. a
Bluetooth mic that just disconnected).

Unknown commands are ignored, so new ones can be added compatibly. The
plugin's embedded web panel (http://localhost:9980) generates these.

`start_stream` / `stop_stream` are the **remote start** commands: they
start/stop the camera itself (not just the connection) and are honoured
only while the app's **Remote start from OBS** option is on. The plugin
sends `start_stream` when the user clicks **Start camera on the phone**
(source properties or web panel), or automatically on receiving a standby
HELLO when its **auto-start** option is enabled — but only if the app was
previously unreachable (just opened/foregrounded), so stopping the stream
on the phone doesn't bounce straight back into streaming. With
**Disconnect when this source isn't shown anywhere** plus auto-start, the
plugin sends `stop_stream` before dropping the connection on hide and
`start_stream` again on show.

### 8 — STATE (app → plugin)
Camera-state snapshot, sent (debounced ~200 ms) whenever a control value
changes and once on connect. Payload: UTF-8 JSON, e.g.

```json
{ "zoom": 2.5, "maxZoom": 10, "exposureBias": -0.5,
  "focusMode": "locked", "lensPosition": 0.4,
  "flashlight": true, "hasFlashlight": true, "camera": "back" }
```

The plugin caches the latest snapshot and serves it at `/api/state` so
remote UIs mirror the app (and vice versa) regardless of where a change
was made.

The snapshot also carries white-balance and manual-exposure state
(`whiteBalanceMode`/`whiteBalanceTemperature`, `exposureMode`/`iso`/
`shutterSeconds` with their ranges, plus `supportsWhiteBalanceLock` and
`supportsManualExposure` so UIs hide what the camera lacks) and the
capture format (`resolution`/`fps`/`codec` with `resolutions`/
`frameRates`/`codecs` capability lists for `set_format` pickers).

While the phone mic streams as the source's audio (packet type 10), the
snapshot additionally carries `micEnabled: true`, the selected `mic` id,
and the selectable `mics` list (`[{ "id", "name" }, …]`) for the `mic`
command's pickers. Absent otherwise, so remote UIs key their mic row off
`micEnabled`.

### 9 — AUDIO (app → plugin)
Reference audio for lip-sync auto-calibration. Payload: raw **16 kHz mono
signed-16-bit little-endian PCM**; `pts` = capture time of the first
sample, in the same clock domain as video frames. Sent in ~100 ms chunks
only while the app's "Auto lip-sync reference" option is on.

This audio is **never played out**. The plugin converts each chunk's pts
to its own clock (via the TIMESYNC offset), builds an amplitude envelope,
and cross-correlates it against the OBS microphone the user selected. The
correlation peak is the mic's true latency `L_mic`; the applied sync
offset is then `L_v − L_mic` (video latency minus mic latency), measured
directly with no manual entry. Low-confidence windows (silence) hold the
last value.

### 10 — SCREEN_AUDIO (app → plugin)
**Playable audio**, output as the source's audio in OBS via
`obs_source_output_audio` — unlike type 9, this is not a lip-sync
reference. Payload: raw **48 kHz stereo signed-16-bit little-endian
interleaved PCM**; `pts` = capture time of the first sample, in the same
clock domain as the video frames, so OBS keeps A/V aligned.

Two senders use it:

- the **broadcast extension** (`kind: "screen"`): the mirrored screen's
  system audio. Microphone audio is intentionally omitted there (a
  streamer mics themselves in OBS; the phone mic would double it).
- the **camera app** (`kind: "camera"`), only while its **Send phone mic
  to OBS** option is on: the phone microphone as the camera source's
  audio — the phone as a wireless mic. Mutually exclusive with the
  type-9 lip-sync reference (one mic, one role).

### 11 — DIAG (app/extension → plugin)
Optional diagnostics. Payload: a short UTF-8 text line summarising the
sender's internal counters (screen samples in, frames encoded/sent, bytes,
connection state). The plugin echoes it into the OBS log (prefixed
`[lenslink][phone]`) when the source's **Verbose diagnostics** option is on,
so both ends of the pipeline appear together — the broadcast extension has
no console of its own. Purely informational; a receiver may ignore it.

## Pairing

Optional, app-side ("Require pairing", off by default). When on, the
app's HELLO carries `"auth": "required"`, and the app sends **no video,
audio, or state** — and honours no control commands — until the
connection authenticates. Three CONTROL commands (plugin → app) exist
for it:

```json
{ "cmd": "auth", "token": "…" }      // present a stored pairing token
{ "cmd": "pair_request" }            // ask the app to display a PIN
{ "cmd": "pair", "pin": "123456" }   // redeem the user-entered PIN
```

The app answers on the STATE channel with `{ "auth": "ok" }` (plus a
fresh `"pairedToken"` after a successful `pair`, which the plugin
persists in the source settings and presents on future connects) or
`{ "auth": "denied" }`. Auth-result STATEs are consumed by the plugin
and never stored/served via `/api/state`. A pairing attempt dies with
its connection; timesync and pings flow regardless (they leak nothing).

Tokens gate access, not confidentiality: the stream itself is still
plaintext on the LAN. Wire encryption remains a roadmap item.

## Discovery (Bonjour)

Whenever the app's listener is up (streaming, standby, or a screen
broadcast), it is advertised over Bonjour as **`_lenslink._tcp`** with the
device's name as the service instance. The plugin performs a one-shot
mDNS-SD browse (RFC 6762 §5.1: PTR query from an ephemeral port with the
unicast-response bit, answers arrive unicast) when the source's
properties open, and offers discovered phones by name in the Phone
field. Only the PTR answer's instance label and the responder's source
address are used — the wire port is fixed — so no SRV/A parsing or
multicast group membership is required. Typing an IP directly still
works exactly as before.

## USB transport

The packet protocol is identical over USB. The plugin reaches the app's
listener through the usbmuxd protocol (Apple Mobile Device Service on
`localhost:27015` on Windows; the `/var/run/usbmuxd` socket on
macOS/Linux): `ListDevices` → first attached device → `Connect` with the
port in network byte order. After a successful `Connect` result the mux
socket is a raw byte pipe and the normal packet stream begins with the
app's HELLO.
