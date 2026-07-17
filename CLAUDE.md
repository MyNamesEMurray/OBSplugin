# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

Two deliverables share this repo and talk over a custom TCP wire protocol:

- **OBS Studio plugin** (`obs-plugin/`, plain C, CMake) — registers two OBS
  sources, **LensLink Camera** and **LensLink Screen**. It *dials* the phone
  (LAN IP or USB via usbmuxd), decodes the H.264/HEVC stream with FFmpeg
  (GPU when available), and outputs frames to OBS.
- **iOS app** (`ios-app/`, SwiftUI, XcodeGen — no `.xcodeproj` checked in) —
  captures with AVFoundation, hardware-encodes with VideoToolbox, and
  *listens* on TCP port 9979 on the device.

Read the matching doc before touching an area:

- `docs/PROTOCOL.md` — the wire protocol. **Any protocol change must update
  this spec and stay compatible with the version negotiation it describes.**
- `docs/UI_DESIGN.md` — design system for all UI surfaces, the single source
  of truth. Its tokens are implemented twice — `ios-app/Sources/DesignSystem.swift`
  and the web panel in `obs-plugin/src/web-control.c` — change both together.
- `docs/PERFORMANCE.md` — where the cycles go and a "don't regress these"
  list (zero-copy receive path, no extra threads/timers, drop-don't-queue
  backpressure). Performance PRs must quote benchmark numbers
  (`tools/bench-report.py`).
- `docs/DEVELOPMENT.md` — architecture, repo layout, CI, release automation.

## Commands

There is no test suite; verification is compile checks + CI + manual device
testing. The `.claude/skills/verify` skill has per-surface verification
recipes for this container (including how to test the App Store Connect
scripts against a local mock).

### Swift changes: syntax check (works in this Linux environment)

This environment is Linux, so the iOS app cannot be compiled here (SwiftUI
and the other Apple frameworks only exist in Xcode on macOS). After editing
any Swift file, run:

```bash
ios-app/syntax-check.sh
```

It parse-checks every app source (`swiftc -parse`), catching syntax-level
errors before a push. On first run it downloads a Linux Swift toolchain
into `~/.cache/lenslink-swift` (~840 MB, ~40 s); later runs take seconds.
It cannot catch type errors, wrong API names, or iOS-availability
mistakes — only the macOS CI job can. Deployment target is iOS 15: avoid
iOS 16+-only SwiftUI API (e.g. use `NavigationView`, not
`NavigationStack`) unless gated by `@available`.

### OBS plugin: build (works in this Linux environment)

```bash
sudo apt install cmake build-essential pkg-config \
     libobs-dev libavcodec-dev libavutil-dev qt6-base-dev

cd obs-plugin
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="-Wall -Wextra -Werror"
cmake --build build
```

CI builds with `-Wall -Wextra -Werror`, so use those flags locally to match.
`qt6-base-dev` is optional — the status-bar/dock UI (`frontend-ui.cpp`)
compiles only when Qt6 is found; the rest must build and work without it.

### iOS app: full build (macOS only)

```bash
brew install xcodegen
cd ios-app && xcodegen generate && open LensLink.xcodeproj
```

Details (signing, older Xcode): `ios-app/BUILDING.md`.

## CI and releases

- CI (`.github/workflows/build.yml`) runs on **pull requests only**, not
  branch pushes, and builds only the areas the PR touches (plugin on
  Ubuntu/Windows/macOS with `-Werror`; the app as an unsigned device build
  on macOS). PRs merge automatically once the required checks pass.
- **Merging to `main` auto-releases** when `obs-plugin/`, `ios-app/`, or
  `installer/` changed: patch bump by default, or the bump named by a git
  trailer on its own line in any commit of the PR — `Release-Bump: minor`,
  `Release-Bump: major`, or `Release-Skip: true` (no release). Mind this
  when writing commit messages. The TestFlight upload runs only when the
  merge touched `ios-app/`.
- **Never manually dispatch** `testflight.yml` or anything that creates
  releases/tags — a run uploads a real build to TestFlight / publishes a
  real release.

## Architecture

### Wire protocol (spec: docs/PROTOCOL.md)

One TCP connection. Every packet: 20-byte header (`OBSC` magic, version,
type, flags, pts in **nanoseconds**, payload size) + payload. The app sends
HELLO on connect; its `kind` field (`"camera"` or `"screen"`) routes the
stream to the matching source type, and `"standby": true` means "app open
and idle, waiting for remote start". Points that shape the code:

- VIDEO carries Annex B access units; keyframes must be self-contained
  (parameter sets prepended) so a decoder can join mid-stream.
- TIMESYNC_REQ/RESP is an NTP-style clock-offset exchange (1 Hz) that
  underpins the latency figure, lip-sync calibration, and health readouts.
- CONTROL (plugin → app) is one JSON command per packet (zoom, focus,
  `start_stream`/`stop_stream`, `set_format`, `mic`, …). Unknown commands
  are ignored, so new ones are backwards-compatible.
- STATE (app → plugin) is a debounced camera-state snapshot; the plugin
  caches it and serves it at `/api/state`, which is how the app UI, web
  panel, and source properties stay in lock-step.
- AUDIO (type 9) is a lip-sync timing reference that is **never played**;
  SCREEN_AUDIO (type 10) is playable source audio. Don't confuse them.

### OBS plugin (obs-plugin/src/)

- `plugin-main.c` registers both sources; both are implemented in
  `ios-camera-source.c` (dial loop, packet parsing, latency, lip-sync glue,
  source properties). One dial-loop thread per source — timesync, control
  forwarding, and diagnostics piggyback on it; don't add threads or timers.
- `h264-decoder.c` decodes via libavcodec (GPU with software fallback).
  `gpu-frame.c` is the GPU-pipeline beta: when enabled, the source-info
  struct is patched *before registration* (self-rendering sync source
  instead of async pixel-pusher) — that's why toggling it requires an OBS
  restart.
- Transports: plain LAN dial, `usbmux.c` (usbmuxd client for USB),
  `mdns.c` (one-shot Bonjour browse of `_lenslink._tcp` for the Phone
  dropdown).
- `web-control.c` serves the browser control panel on `localhost:9980`
  plus `/api/state` and `/api/control`.
- `plugin-settings.c` holds plugin-wide settings (Tools → LensLink
  Settings); global settings are never duplicated in per-source properties.
- Locale strings: `obs-plugin/data/locale/en-US.ini`.

### iOS app (ios-app/Sources/)

- `Streamer.swift` is the `@MainActor` hub gluing `CameraManager`
  (AVFoundation capture) → `VideoEncoder` (VideoToolbox, AVCC→Annex B) →
  `StreamClient` (Network.framework listener on 9979, packet framing,
  Bonjour advertising). Its `Status` enum defines the canonical status
  word and colour that every UI surface reuses.
- `BroadcastExtension/SampleHandler.swift` is the ReplayKit
  broadcast-upload extension for screen mirroring — a separate process
  that reuses `StreamClient`/`VideoEncoder` and speaks the same protocol
  with `kind: "screen"` (system audio only, no mic, by design).
- The app only streams while foregrounded (iOS suspends background camera
  capture); the remote-start machinery (standby HELLO + `start_stream`)
  exists because of this.

## Conventions

- C follows OBS style (tabs); Swift uses 4 spaces (`.editorconfig` covers
  the basics). Match the style around you.
- UI wording follows `docs/UI_DESIGN.md` vocabulary — e.g. always
  "Flashlight", never "Torch"; status words come from `Status.displayName`,
  never ad-hoc strings.
