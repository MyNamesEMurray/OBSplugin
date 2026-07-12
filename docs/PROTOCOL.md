# OBS iOS Camera — Wire Protocol (version 1)

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
{ "name": "Emma's iPhone", "app": "OBSCam", "protocol": 1 }
```

### 2 — VIDEO_CONFIG
Sent after HELLO and again whenever the capture format changes.
Payload: UTF-8 JSON, e.g.

```json
{ "codec": "h264", "width": 1280, "height": 720, "fps": 30 }
```

`codec` is `"h264"` or `"hevc"` and selects the plugin's decoder; a codec
change mid-stream resets the decoder (the next keyframe re-initializes it).
Dimensions/fps are informational; the authoritative values come from the
bitstream parameter sets.

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
{ "cmd": "torch", "on": true }
{ "cmd": "flip" }
```

Unknown commands are ignored, so new ones can be added compatibly. The
plugin's embedded web panel (http://localhost:9980) generates these.

### 8 — STATE (app → plugin)
Camera-state snapshot, sent (debounced ~200 ms) whenever a control value
changes and once on connect. Payload: UTF-8 JSON, e.g.

```json
{ "zoom": 2.5, "maxZoom": 10, "exposureBias": -0.5,
  "focusMode": "locked", "lensPosition": 0.4,
  "torch": true, "hasTorch": true, "camera": "back" }
```

The plugin caches the latest snapshot and serves it at `/api/state` so
remote UIs mirror the app (and vice versa) regardless of where a change
was made.

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

## USB transport

The packet protocol is identical over USB. The plugin reaches the app's
listener through the usbmuxd protocol (Apple Mobile Device Service on
`localhost:27015` on Windows; the `/var/run/usbmuxd` socket on
macOS/Linux): `ListDevices` → first attached device → `Connect` with the
port in network byte order. After a successful `Connect` result the mux
socket is a raw byte pipe and the normal packet stream begins with the
app's HELLO.
