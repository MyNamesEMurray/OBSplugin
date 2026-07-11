# OBS iOS Camera

Use your iPhone or iPad camera as a webcam **directly inside OBS Studio** —
no virtual camera drivers, no RTMP server, just your local network.

Two components:

| Component | Path | What it does |
|-----------|------|--------------|
| **OBS plugin** (`iOS Camera` source) | [`obs-plugin/`](obs-plugin/) | Listens on TCP port 9977, decodes the incoming H.264 stream with FFmpeg, and renders it as a normal OBS video source. Also answers LAN discovery probes on UDP 9978. |
| **iOS app** (`OBSCam`) | [`ios-app/`](ios-app/) | Captures the camera with AVFoundation, hardware-encodes to H.264 with VideoToolbox, and streams it to the plugin over TCP. |

```
┌─────────────── iPhone ───────────────┐        ┌──────────── Computer ────────────┐
│ AVCaptureSession → VideoToolbox H.264 │  TCP   │ TCP server → libavcodec decode → │
│ → Annex B framing → NWConnection      │ ─────▶ │ obs_source_output_video()        │
└──────────────────────────────────────┘  :9977  └──────────────────────────────────┘
                     ▲  UDP broadcast discovery (:9978)  │
                     └───────────────────────────────────┘
```

## Quick start

1. **Build & install the plugin** — see [`obs-plugin/BUILDING.md`](obs-plugin/BUILDING.md).
2. **Build & run the iOS app** — see [`ios-app/BUILDING.md`](ios-app/BUILDING.md).
3. In OBS: *Sources → + → iOS Camera*. Leave the port at 9977.
4. On the phone (same Wi-Fi): open OBSCam, tap your computer in the
   discovered list (or type its IP), pick camera/resolution/frame rate, and
   tap **Start streaming to OBS**.

The video appears in the OBS source within a second or two. Disconnecting
(or backgrounding the app) blanks the source.

## Features

- 720p / 1080p at 30 or 60 fps, hardware-encoded (low battery/CPU cost)
- Front or back camera, mirrored front-camera preview
- Automatic OBS discovery on the LAN (UDP broadcast), with manual IP fallback
- Reconnect-friendly: keyframes every 2 s carry SPS/PPS, so OBS can join or
  recover mid-stream
- Cross-platform plugin (Linux / macOS / Windows), plain C against libobs +
  FFmpeg

## Repository layout

```
obs-plugin/            C plugin for OBS Studio (CMake)
  src/protocol.h       wire-protocol constants + header parsing
  src/ios-camera-source.c   the OBS source: TCP server, discovery, packet loop
  src/h264-decoder.c   libavcodec H.264 → obs_source_frame
ios-app/               SwiftUI companion app (XcodeGen project)
  Sources/H264Encoder.swift   VideoToolbox encode + AVCC→Annex B
  Sources/StreamClient.swift  Network.framework TCP client + framing
  Sources/DiscoveryClient.swift  UDP broadcast discovery
docs/PROTOCOL.md       wire protocol specification (version 1)
```

## Protocol

A tiny length-prefixed packet protocol over one TCP connection (video is
H.264 Annex B access units; timestamps in nanoseconds). Full spec in
[`docs/PROTOCOL.md`](docs/PROTOCOL.md).

## Limitations & roadmap

- Video only — microphone audio is a natural next step (`OBSC_PKT_AUDIO`).
- One connected device per source (add multiple sources on different ports
  for multiple phones).
- The stream is unencrypted on your LAN; intended for trusted home/studio
  networks.
