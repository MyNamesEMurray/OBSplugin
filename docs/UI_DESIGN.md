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
(dead) camera controls with a single accent **Start camera** button;
while live, the panel ends in a red **Stop camera** button (Stop stays
the one destructive control, mirroring the app's red Stop chip).

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
| Exposure mode | segmented **AE / Manual**               | When Manual: the bias slider is replaced by ISO (`dial.min`/`dial.max`) and Shutter (`tortoise`/`hare`, log-scale, readout `1/125`) rows. Hidden if unsupported |
| White balance | segmented **AWB / Lock**                | When Lock: a colour-temperature slider (2500–8000 K, readout `5600K`). Hidden if unsupported |
| Flashlight  | `bolt.fill` (toggle; hidden if unavailable)  | Chip, `glassChipOn` when on. **Always labelled "Flashlight," never "Torch."** |
| Lens        | `camera.aperture` menu                     | Menu of the device's real lenses; check on the active one |
| Flip        | `arrow.triangle.2.circlepath.camera`      | Quick front/back |
| Stop        | `stop.fill`                                | Red chip; the only destructive control |
| Dim (app)   | `moon.fill`                                | App-only battery saver |
| Stats (app) | `gauge`                                    | App-only toggle; shows a health pill (`60 fps · 11.9 Mb/s · 0 dropped`, monospaced) under the status bar |

---

## 6. Screen specifications

### 6.1 App — Setup screen
Grouped form, top to bottom — parallel modules, each naming the OBS source
it feeds and ending in the same full-width accent action button:

1. **Banner** — the wordmark as the screen's title (no navigation bar;
   nothing is ever pushed).
2. **Connect** — status dot (`status.tint`) + `status.displayName` with
   the phone's Wi-Fi IP on the same line (monospaced, tap-to-copy), and a
   "How to connect" disclosure with the two setup steps — collapsible, and
   it stays collapsed once read.
3. **Camera** — Lens, Resolution, Frame rate, Codec pickers (each filtered
   to what the selected lens supports), then **Start camera stream**. A
   contextual "Open Settings" button appears only if a permission was
   denied.
4. **Screen mirror** — a "Screen mirror tools" diagnostics disclosure,
   then **Start screen broadcast** (the system broadcast picker is
   stretched invisibly over our button face — iOS won't start a broadcast
   any other way).
5. **Tail** — an **Options** row that presents the Options sheet, the
   GitHub link, and the version line in the footer.

Long explanations don't belong on this screen: each module's footer is at
most a sentence or two, so the form stays close to one screenful.

**Options sheet.** The behaviour toggles live in a sheet (`OptionsView`)
so the main screen stays short: **Remote start from OBS**, **Dim screen
while streaming**, and a **Microphone** group (**Send phone mic to OBS** /
**Auto lip-sync reference** — mutually exclusive; turning one on turns the
other off). Each toggle sits in its own section with a short footer
directly beneath it — never one combined wall-of-text footer for all of
them.

### 6.2 App — Live screen
Full-screen black; camera preview `resizeAspect`; content over it:

- **Top bar:** status pill (dot + word, left) · Stats button · Dim button ·
  Stop button (red). Glass chips. With Stats on, a health pill (fps ·
  Mb/s · dropped) sits under the bar, leading-aligned.
- **Bottom panel** (`glassPanel`, radius 16): zoom row, exposure rows
  (AE/Manual segmented sharing the bias-slider row; ISO + Shutter rows in
  Manual), white-balance row (AWB/Lock + temperature slider), then a
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

### 6.4 OBS status bar & dock

A third surface lives inside OBS itself (Qt, `frontend-ui.cpp`): a
one-line health readout in the main window's status bar
(`LensLink: <device> 60 fps · 11.9 Mb/s · 43 ms`, live sources joined by
`  |  `, hidden when nothing is
connected; "ready (camera idle)" during standby) and a dockable
**LensLink** panel listing every source with its device, status, and
rates. These render with OBS's native theme rather than our palette —
inside OBS's chrome, OBS's design language wins; our vocabulary (the
status wording, `fps · Mb/s · ms` ordering, monospace-style numerals)
is what carries over.

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
