# OBS iOS Camera

Use your iPhone or iPad camera as a webcam **directly inside OBS Studio** —
no virtual camera drivers, no RTMP server, just your local network.

Two components:

| Component | Path | What it does |
|-----------|------|--------------|
| **OBS plugin** (`iOS Camera` source) | [`obs-plugin/`](obs-plugin/) | Connects to the phone (LAN IP or USB via usbmuxd), decodes the incoming H.264/HEVC stream with FFmpeg (GPU when available), and renders it as a normal OBS video source. |
| **iOS app** (`OBSCam`) | [`ios-app/`](ios-app/) | Captures the camera with AVFoundation, hardware-encodes with VideoToolbox, and serves the stream to the plugin over TCP (port 9979 on the device). |

```
┌────────────── iPhone ───────────────┐         ┌─────────── Computer ────────────┐
│ AVCaptureSession → VideoToolbox     │   TCP   │ plugin dials the phone (LAN IP  │
│ H.264/HEVC → Annex B → NWListener   │ ◀────── │ or USB via usbmuxd) → decode →  │
│ (port 9979 on the device)           │ ──────▶ │ obs_source_output_video()       │
└─────────────────────────────────────┘  video  └─────────────────────────────────┘
```

## Quick start

1. **Build & install the plugin** — see [`obs-plugin/BUILDING.md`](obs-plugin/BUILDING.md).
2. **Build & run the iOS app** — see [`ios-app/BUILDING.md`](ios-app/BUILDING.md).
3. On the phone: open OBSCam, pick camera/resolution/frame rate, and tap
   **Start** — the app shows the phone's IP address.
4. In OBS: *Sources → + → iOS Camera*, enter that IP as the **Phone IP**
   (or plug in a USB cable and pick Connection → **USB cable** — no Wi-Fi
   needed). OBS connects to the phone within a couple of seconds.

The video appears in the OBS source within a second or two. Disconnecting
(or backgrounding the app) blanks the source.

## Features

- **Wi-Fi or USB**: OBS connects to the phone — enter the IP the app shows
  (no inbound firewall rules on the PC), or use the Lightning/USB-C cable
  via Apple's device mux (usbmuxd; on Windows install iTunes). USB needs
  no network at all and charges the phone while streaming.
- **H.264 or HEVC**, hardware-encoded (HEVC ~40% smaller at the same
  quality; automatic H.264 fallback on devices without HEVC encode)
- **Lens selection** — Main, Ultra Wide, Telephoto, Front: dynamically
  enumerated from the device's actual cameras, switchable live while
  streaming (picker in settings, lens menu in the streaming view)
- **720p / 1080p / 4K at 30 or 60 fps** — the app reads each lens's real
  format list and only offers combinations the hardware supports
- **Automatic lip sync**: pick any OBS audio source in the iOS Camera
  properties and the plugin continuously sets its sync offset to the
  measured camera latency (minus a configurable audio-device latency)
- **Adaptive bitrate**: when the link congests, the app backs the encoder
  off within a second and recovers gradually — no more latency spirals on
  weak Wi-Fi
- **Full-screen streaming UI** with pinch zoom, tap-to-focus, exposure
  bias, focus lock (AF/manual lens position), torch, camera flip, and
  battery-saving auto-dim
- **Remote camera control from the PC**: the plugin serves a control panel
  at http://localhost:9980 (zoom / exposure / focus / torch / flip) that
  drives the phone over the stream connection
- Mirrored front-camera preview
- Reconnect-friendly: keyframes every 2 s carry SPS/PPS, so OBS can join or
  recover mid-stream
- Cross-platform plugin (Linux / macOS / Windows), plain C against libobs +
  FFmpeg

## Repository layout

```
obs-plugin/            C plugin for OBS Studio (CMake)
  src/protocol.h       wire-protocol constants + header parsing
  src/ios-camera-source.c   the OBS source: dial loop, latency, lip sync
  src/h264-decoder.c   libavcodec H.264/HEVC → obs_source_frame (GPU-capable)
  src/usbmux.c         usbmuxd client (USB transport)
  src/web-control.c    browser control panel (http://localhost:9980)
ios-app/               SwiftUI companion app (XcodeGen project)
  Sources/VideoEncoder.swift  VideoToolbox encode + AVCC→Annex B
  Sources/StreamClient.swift  Network.framework listener + framing
  Sources/StreamingView.swift full-screen streaming UI + camera controls
docs/PROTOCOL.md       wire protocol specification (version 1)
```

## Protocol

A tiny length-prefixed packet protocol over one TCP connection (video is
H.264 Annex B access units; timestamps in nanoseconds). Full spec in
[`docs/PROTOCOL.md`](docs/PROTOCOL.md).

## Latency

The plugin continuously measures **capture→decode latency** (camera
timestamp on the phone to decoded frame in OBS) using NTP-style clock sync
over the stream connection — accurate to about half the link round-trip
(±1–2 ms). It's reported every 5 s in the OBS log
(`[ios-camera] capture->decode latency: …`) and in the source's Status
field (reopen Properties to refresh).

That figure excludes OBS's own render/compositing and your display. For
true glass-to-glass, point the phone at a millisecond stopwatch on your
monitor and screenshot the OBS preview next to the stopwatch — the
difference is the real end-to-end number.

Low-latency defaults: the source renders the newest frame immediately
("Low latency mode" checkbox, on by default — turn it off if you prefer
smoother pacing over minimum delay), decoding runs on the GPU when
available (D3D11VA on Windows, VideoToolbox on macOS, VAAPI on Linux;
automatic software fallback, toggleable via "Hardware decoding"), the
encoder prioritizes speed with no frame reordering, and the app drops
rather than queues frames when the link stalls. USB mode
typically shaves a few more milliseconds over Wi-Fi and is immune to
Wi-Fi jitter.

## Continuous integration

Every push/PR runs [`.github/workflows/build.yml`](.github/workflows/build.yml):

- **OBS plugin** — built on Ubuntu against libobs + FFmpeg with
  `-Wall -Wextra -Werror`; the compiled `ios-camera-source.so` (plus its
  `data/` folder) is uploaded as a downloadable artifact on each run.
- **iOS app** — the Xcode project is generated with XcodeGen and compiled
  for the iOS Simulator on a macOS runner. This validates the Swift code but
  does not produce an installable app: iOS device builds must be signed, so
  installing on your phone still goes through Xcode with your Apple ID (see
  `ios-app/BUILDING.md`). To ship signed builds from CI later, add an Apple
  signing certificate + provisioning profile as repo secrets and a
  `fastlane`/`xcodebuild -exportArchive` step.

## Limitations & roadmap

- Video only — microphone audio is a natural next step (`OBSC_PKT_AUDIO`).
- One connected device per source.
- The stream is unencrypted on your LAN; intended for trusted home/studio
  networks.
