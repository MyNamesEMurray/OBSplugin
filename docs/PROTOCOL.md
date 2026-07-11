# OBS iOS Camera — Wire Protocol (version 1)

The iOS app is the **client**; the OBS plugin is the **server**. Transport is a
single TCP connection on the plugin's configured port (default **9977**).

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

The plugin treats this as informational; the authoritative dimensions come
from the H.264 bitstream (SPS).

### 3 — VIDEO
Payload: one H.264 **access unit in Annex B format** (start-code delimited
NAL units). Keyframe packets must set the keyframe flag and must be
self-contained: SPS and PPS NAL units are prepended before the IDR slice so a
decoder can join mid-stream.

`pts` is the capture presentation timestamp in nanoseconds. It only needs to
be monotonic; OBS re-bases async timestamps itself.

### 4 — PING
Optional keep-alive, empty payload, sent by the client every ~2 s while idle.
The server ignores it.

## Discovery (optional)

The plugin listens for UDP datagrams on port **9978** (discovery port =
video port + 1). A client broadcasts the ASCII string:

```
OBSC_DISCOVER
```

to `255.255.255.255:9978`. Each plugin instance replies to the sender with:

```
OBSC_HERE:<tcp_port>:<host_name>
```

Discovery is best-effort; manual host/port entry always works.
