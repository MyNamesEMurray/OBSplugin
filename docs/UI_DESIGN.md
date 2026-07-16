# LensLink — UI Design System

This document defines the visual and interaction language shared by the two
user-facing surfaces of the project:

1. **The iOS app (LensLink)** — runs on the phone; two screens: **Setup**
   (before streaming) and **Live** (full-screen while streaming).
2. **The plugin's web control panel** — served on the PC at
   `http://localhost:9980`; a remote control surface for the operator at
   the computer.

They serve different vantage points (phone vs. PC) but must feel like one
product: same palette, same control metaphors, same status vocabulary.
This document is the single source of truth; the app's `DesignSystem.swift`
and the plugin's `web-control.c` page both implement the tokens below, and
any change here should be reflected in both.

---

## 1. Principles

- **The video is the interface.** On the Live screen and the web panel the
  camera image is the hero; controls float over it in translucent glass and
  never obscure the frame more than necessary.
- **One glance = status.** A single coloured dot + one word communicates
  connection state identically everywhere.
- **Same control, same shape.** Zoom, exposure, focus, flashlight, lens and
  flip look and behave the same on the phone and in the browser.
- **Calm by default.** Muted surfaces, one accent colour, restrained
  motion. Nothing pulses or animates unless it reflects real state change.
- **Reversible & non-destructive.** Only Stop is destructive (red); every
  other control is a safe, adjustable value.

---

## 2. Colour

All colours are given as hex so the native app and the web page render
identically. The status palette intentionally mirrors iOS system semantic
colours so it also looks native on the phone.

### Accent
| Token            | Hex       | Use |
|------------------|-----------|-----|
| `accent`         | `#3D7BFF` | Primary actions, active toggles, slider fills, selection |
| `accentPressed`  | `#2E63E6` | Pressed/active state of accent controls |

### Status (dot + label)
| State        | Token         | Hex       | Label            |
|--------------|---------------|-----------|------------------|
| Idle         | `idleGrey`    | `#8E8E93` | "Not connected"  |
| Standby      | `connectAmber`| `#FF9F0A` | "OBS connected — ready" |
| Connecting   | `connectAmber`| `#FF9F0A` | "Waiting for OBS…" |
| Live         | `liveGreen`   | `#30D158` | "Live"           |
| Error        | `errorRed`    | `#FF453A` | *(the message)*  |

Standby is the remote-start state: the app is idle but OBS is connected
and can start the camera. It shares the amber of Connecting — both mean
"linked, not yet live". On the web panel the standby state replaces the
(dead) camera controls with a single accent **Start camera** button.

The status **word and colour are defined once** (`Streamer.Status.displayName`
/ `.tint` in the app) and reused by every view; the web panel maps the
plugin's status string to the same palette.

### Surfaces (dark / over-video)
| Token          | Value                                   | Use |
|----------------|-----------------------------------------|-----|
| `pageBg`       | `#0E0F13`                               | Web page background; app preview backdrop is pure black |
| `glassPanel`   | black @ 55% + blur (`.regularMaterial`) | Floating control panels |
| `glassChip`    | white @ 12%                             | Circular control buttons (idle) |
| `glassChipOn`  | `accent` @ 90%                          | Active/toggled control buttons |
| `hairline`     | white @ 8%                              | Panel borders on the web |

### Text on glass
| Token            | Value          |
|------------------|----------------|
| `textPrimary`    | `#FFFFFF`      |
| `textSecondary`  | white @ 60%    |

### Setup screen (light/dark system)
The Setup screen uses the **native grouped-form** styling (system
background, `.insetGrouped` lists) so it feels like a standard iOS settings
screen and adapts to light/dark automatically. Only the accent colour and
the status dot/label carry the brand palette into it.

---

## 3. Typography

Native system font (SF on Apple, `system-ui` on web).

| Role            | Size / weight            | Notes |
|-----------------|--------------------------|-------|
| Screen title    | 20 semibold              | "LensLink", panel headings |
| Body            | 15–17 regular            | Descriptions, list rows |
| Label / caption | 13 regular               | Secondary text, field labels |
| Numeric readout | 13–15 **monospaced digits** | Zoom `2.0×`, exposure `+0.3`, latency `57 ms` — monospaced so values don't jitter while dragging |

---

## 4. Spacing, radius, sizing

- **Spacing scale:** 4, 8, 12, 16, 20, 24. Use these steps only.
- **Radius:** panel `16`, chip/segment `12`, pill/status `full`.
- **Control button:** 44×44 pt/px circular hit target (glass chip),
  22 pt icon.
- **Panel padding:** 16 (14 acceptable on very small screens).
- **Slider row:** leading icon · slider · trailing icon · readout (fixed
  40–48 pt width, right-aligned, monospaced).

---

## 5. Iconography & control metaphors

SF Symbols on iOS; the web mirrors the same glyph meaning (unicode/inline
SVG) and identical layout. Every control appears in the same order on both
surfaces.

| Control     | Icon(s)                                   | Pattern |
|-------------|-------------------------------------------|---------|
| Zoom        | `minus.magnifyingglass` / `plus.magnifyingglass` | Slider 1×…max, readout `N.N×` |
| Exposure    | `sun.min` / `sun.max`                     | Slider −range…+range, readout `±N.N` |
| Focus       | segmented **AF / Lock**                   | When Lock: a lens-position slider (0=near, 1=far) |
| Flashlight  | `bolt.fill` (toggle; hidden if unavailable)  | Chip, `glassChipOn` when on. **Always labelled "Flashlight," never "Torch."** |
| Lens        | `camera.aperture` menu                     | Menu of the device's real lenses; check on the active one |
| Flip        | `arrow.triangle.2.circlepath.camera`      | Quick front/back |
| Stop        | `stop.fill`                                | Red chip; the only destructive control |
| Dim (app)   | `moon.fill`                                | App-only battery saver |
| Stats (app) | `gauge`                                    | App-only toggle; shows a health pill (`60 fps · 11.9 Mb/s · 0 dropped`, monospaced) under the status bar |

---

## 6. Screen specifications

### 6.1 App — Setup screen
Grouped form, top to bottom:

1. **Status** — dot (`status.tint`) + `status.displayName`.
2. **How to connect** — the phone's Wi-Fi IP in large monospaced text
   (tap-to-copy), one line telling the operator to enter it in OBS, and a
   note that a USB cable also works.
3. **Camera** — Lens, Resolution, Frame rate, Codec pickers (each filtered
   to what the selected lens supports).
4. **Options** — Remote-start and Dim-screen toggles.
5. **Lip sync** — Auto lip-sync reference toggle + explanation.
6. **Action** — a single filled **accent** "Start streaming to OBS" button;
   a contextual "Open Settings" button appears only if a permission was
   denied.

### 6.2 App — Live screen
Full-screen black; camera preview `resizeAspect`; content over it:

- **Top bar:** status pill (dot + word, left) · Stats button · Dim button ·
  Stop button (red). Glass chips. With Stats on, a health pill (fps ·
  Mb/s · dropped) sits under the bar, leading-aligned.
- **Bottom panel** (`glassPanel`, radius 16): zoom row, exposure row, then a
  control row of Focus segmented + (lens-position slider when **Lock**, else
  a flexible spacer) + Flashlight + Lens menu + Flip.
- **Gestures:** pinch anywhere = zoom; tap = focus/expose at point. In
  **AF** the focus row carries no inline label — tap-to-focus is a gesture,
  not on-screen text (a label there crowded the row and wrapped badly).
- **Dim overlay:** after 10 s idle (if enabled) the screen goes near-black
  with a small "Streaming — tap to wake" hint and lowered brightness; any
  tap restores.

### 6.3 Web control panel
Dark page (`pageBg`), single centered column (max ~440 px):

1. **Header:** "LensLink Camera" title + status pill (dot + text) mapped from the
   plugin's status/latency line to the shared palette.
2. **Controls** in the *same order as the app's Live panel*: Zoom row,
   Exposure row, Focus (AF/Lock + lens slider), then a chip row of
   Flashlight · Lens · Flip.
3. All controls send `CONTROL` packets; the panel polls `/api/state` and
   mirrors app-side changes (pausing while the operator is interacting), so
   the two stay in lock-step.

The web panel deliberately shows the **plugin's** connection/latency status
(the PC's vantage point), not the phone app's status — but styled with the
identical pill, palette and control language.

---

## 7. Motion

- Toggles / dim / selection: 200 ms ease.
- Sliders: track the finger live (no animation lag).
- Status colour changes: cross-fade 200 ms.
- Nothing loops or pulses; motion always encodes a real state change.

---

## 8. Reconciliation checklist (this revision)

- [x] Status **word** unified via `Status.displayName` (was two different
      strings for connecting across Setup vs Live).
- [x] Status **colour** unified via `Status.tint` (Live screen previously
      had no error/idle colour).
- [x] Shared **design tokens** (`DesignSystem.swift`) replace ad-hoc
      opacities, radii and paddings.
- [x] "Torch" → **"Flashlight"** everywhere (icon-only in app, labelled on
      web).
- [x] Web panel restyled to the palette, typography, control metaphors and
      ordering above; **Lens selection** added for parity with the app.
