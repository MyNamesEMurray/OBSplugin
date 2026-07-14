# Debugging screen mirroring

Screen mirroring has several moving parts — a broadcast extension that has
no console of its own, a network hop (USB or Wi-Fi), and a decoder on the
plugin side. This branch adds heavy instrumentation so an intermittent
failure points at the exact stage that broke.

Everything lands in **one place: the OBS log** (Help → Log Files → View
Current Log). The phone's own counters are forwarded over the wire and
logged there too, so you don't need to tether the phone to a Mac.

## The main fix

The instrumentation pinned down the most common failure: with **hardware
decoding on**, some screen-mirror resolutions (tall, non-16-aligned, e.g.
886×1918) are silently rejected by the GPU decoder on Windows — it accepts
packets and reports success but never emits a frame, so the source stays
black while the plugin looks "connected." The plugin now **detects this
automatically** (a keyframe went in but no frame came out) and falls back
to software decoding on the spot, requesting a fresh keyframe so the
picture appears within a fraction of a second. No setting to change —
hardware decoding stays the default and just works, recovering itself when
it can't. The counters below are how you confirm that (or diagnose anything
else).

## Turning it on

Diagnostics are **on by default on this branch**. In the LensLink Camera
source properties there's a **"Verbose diagnostics in the OBS log"**
checkbox — leave it on while debugging, turn it off for quiet logs.

## What you'll see

Two heartbeat lines, one from each end, a few seconds apart:

```
[lenslink][diag]  screen t=6s | vid pkt=372 kf=1 dec=360 err=0 5.8 Mbps | aud pkt=58 fr=139k peak=64% | 886x1918 nv12 GPU
[lenslink][phone] vid samp=380 enc=372 kf=1 builds=1 encErr=0 h264 886x1918 aud=60 | net sent=372 kf=1 drop=0 4180KiB aud=58 inflight=1 acc=1 connected
```

- `[lenslink][phone]` is the extension: `samp` = screen frames ReplayKit
  handed us, `enc` = frames encoded, `net sent`/`drop` = frames handed to
  (or dropped before) the socket, `acc` = connections accepted, plus the
  connection state.
- `[lenslink][diag]` is the plugin: `pkt` = video packets received, `kf` =
  keyframes, `dec` = frames actually decoded and pushed to OBS, `err` =
  decode errors, plus the decoded size/format and GPU/CPU.

Watch which counters **move** between heartbeats.

## Reading it

| Symptom in the log | Where it broke | Fix |
|---|---|---|
| No `[lenslink][phone]` lines at all | Extension never reached the plugin, or diagnostics off | Check the source points at this phone; confirm the camera stream isn't already running (one stream per device) |
| Phone `samp=0` | ReplayKit isn't delivering frames | Broadcast didn't really start, or the picker chose the wrong target — restart the broadcast |
| Phone `enc=0` while `samp` climbs | Encoder never built / failed | Look for `encErr` > 0 or an `encoder start failed` line; unusual dimensions |
| Phone `sent=0` while `enc` climbs | Frames encoded but nothing sent | `acc=0` → OBS never connected; otherwise the connection dropped |
| Phone `drop` climbing fast | Network backpressure (the "collapse to 1" case) | Weak Wi-Fi — use USB, or try the HEVC toggle (below) to cut bitrate |
| Plugin `pkt=0` | Plugin isn't receiving | Connection/transport problem — check USB (usbmuxd) or the IP |
| Plugin `pkt` climbs, `kf=0` | No keyframe arrived to join on | Reconnect should force one; if not, the keyframe is being dropped by backpressure |
| **Plugin `pkt` climbs incl. `kf>=1` but `dec=0`** | **Hardware decoder produces nothing — the classic black screen** | **Now auto-handled:** the plugin detects this within ~1/6 s, falls back to software decoding, and asks the phone for a fresh keyframe. You'll see `dec` start climbing a moment later. |
| Plugin `dec` climbs but screen is black in OBS | Decode is fine; it's downstream | An OBS **transform** issue — check the source isn't scaled to nothing; try Right-click → Transform → Reset |
| Plugin `aud pkt` climbs but `peak=0%` | The phone is sending a healthy stream of **silence** | DRM: Apple Music/Spotify/Netflix mute (or pause) themselves during broadcasts. Test with a game or YouTube in Safari. |
| `peak` > 0 but you hear nothing on the PC | OBS received real audio; it just isn't monitored | Mixer meter should be moving. To *hear* it: Advanced Audio Properties → the source's **Audio Monitoring → Monitor and Output** (OBS never plays source audio locally by default). |

The key line is the plugin's automatic fallback (no action needed):

```
[lenslink] hardware decoder produced no frames after 10 packets — falling back to software decoding
```

followed shortly by its success counterpart:

```
[lenslink] first decoded frame: 886x1918 nv12 (GPU) after 412 ms, 5 pkt(s)
```

If you see the "first decoded frame" line, decoding works — any remaining
black is an OBS-side transform/scale problem, not the stream.

## HEVC toggle

Screen content compresses much better in HEVC, so switching can lower the
wire bitrate ~40% and reduce backpressure drops. It's a one-line switch in
`ios-app/BroadcastExtension/SampleHandler.swift`:

```swift
private static let preferHEVC = false   // set true to test HEVC
```

Requires an A10 device (iPhone 7+) for hardware HEVC encoding; it falls
back to H.264 automatically otherwise. The plugin decodes whichever codec
the config announces, so no plugin change is needed. The heartbeat's
`codec` field confirms which one is live.

## Collecting a report

When it fails, grab the OBS log covering the broadcast attempt (Help → Log
Files → **Upload Current Log File**) and note whether it was USB or Wi-Fi
and which app you were mirroring. The heartbeats make the failing stage
obvious from the log alone.
