# Development

Contributor and maintainer notes. End-user documentation is in the
[README](../README.md).

## Components

| Component | Path | What it does |
|-----------|------|--------------|
| **OBS plugin** (`LensLink Camera` source) | [`obs-plugin/`](../obs-plugin/) | Connects to the phone (LAN IP or USB via usbmuxd), decodes the incoming H.264/HEVC stream with FFmpeg (GPU when available), and renders it as a normal OBS video source. |
| **iOS app** (`LensLink`) | [`ios-app/`](../ios-app/) | Captures the camera with AVFoundation, hardware-encodes with VideoToolbox, and serves the stream to the plugin over TCP (port 9979 on the device). |

```
┌────────────── iPhone ───────────────┐         ┌─────────── Computer ────────────┐
│ AVCaptureSession → VideoToolbox     │   TCP   │ plugin dials the phone (LAN IP  │
│ H.264/HEVC → Annex B → NWListener   │ ◀────── │ or USB via usbmuxd) → decode →  │
│ (port 9979 on the device)           │ ──────▶ │ obs_source_output_video()       │
└─────────────────────────────────────┘  video  └─────────────────────────────────┘
```

## Repository layout

```
obs-plugin/            C plugin for OBS Studio (CMake)
  src/protocol.h       wire-protocol constants + header parsing
  src/ios-camera-source.c   the OBS source: dial loop, latency, lip sync
  src/h264-decoder.c   libavcodec H.264/HEVC → obs_source_frame (GPU-capable)
  src/usbmux.c         usbmuxd client (USB transport)
  src/web-control.c    browser control panel (http://localhost:9980)
  src/lipsync.c        audio cross-correlation for lip-sync calibration
ios-app/               SwiftUI companion app (XcodeGen project)
  Sources/VideoEncoder.swift    VideoToolbox encode + AVCC→Annex B
  Sources/StreamClient.swift    Network.framework listener + framing
  Sources/AudioReference.swift  mic capture for lip-sync reference
  Sources/StreamingView.swift   full-screen streaming UI + camera controls
installer/windows/     Inno Setup script for the Windows plugin installer
docs/PROTOCOL.md       wire protocol specification
docs/UI_DESIGN.md      app + web-panel design system
```

## Building

- Plugin: [`obs-plugin/BUILDING.md`](../obs-plugin/BUILDING.md)
- App: [`ios-app/BUILDING.md`](../ios-app/BUILDING.md)

## Protocol

A small length-prefixed packet protocol over one TCP connection (video is
H.264/HEVC Annex B access units; timestamps in nanoseconds). Full spec in
[`docs/PROTOCOL.md`](PROTOCOL.md).

## Continuous integration

Pull requests run [`.github/workflows/build.yml`](../.github/workflows/build.yml):

- **OBS plugin** — built on Ubuntu and Windows against libobs + FFmpeg with
  `-Wall -Wextra -Werror`.
- **iOS app** — the Xcode project is generated with XcodeGen and compiled
  for the iOS Simulator on a macOS runner (validates the Swift; installable
  device builds must be signed).

PRs merge automatically once the required Build checks pass (branch
protection on `main`).

## Releases

Every merge to `main` that touches `obs-plugin/`, `ios-app/`, or
`installer/` automatically tags a version and publishes a GitHub Release
with ready-to-install builds (Windows plugin zip, Linux plugin tarball,
unsigned IPA), via
[`.github/workflows/auto-release.yml`](../.github/workflows/auto-release.yml).
The TestFlight upload piggybacks on that release, but only when the merge
touched `ios-app/`.

The bump is a **git trailer** on its own line in any commit of the merged
PR (case-insensitive):

| Trailer line          | Bump   | Example           |
|-----------------------|--------|-------------------|
| *(none)*              | patch  | v0.3.0 → v0.3.1   |
| `Release-Bump: minor` | minor  | v0.3.0 → v0.4.0   |
| `Release-Bump: major` | major  | v0.3.0 → v1.0.0   |
| `Release-Skip: true`  | none   | no release        |

Because it must be a standalone line, mentioning the keywords in prose
can't trigger a bump. Manual releases also work — push any `v*` tag:

```bash
git tag v0.4.0 && git push origin v0.4.0
```
