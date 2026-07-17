# LensLink — Roadmap

Candidate work, curated and ranked. Nothing here is committed to a
release; items graduate to a GitHub issue when they're picked up. Two
house rules carry over from [`PERFORMANCE.md`](PERFORMANCE.md):
performance changes quote a before/after measurement, and behavior the
users rely on (the invariants listed there) doesn't regress in the name
of speed.

## Performance

Ranked by payoff-per-risk. Items 2–3 came out of the 2026-07 efficiency
audit (see `PERFORMANCE.md` for the cost model behind them).

### 1. Thermal & low-power adaptation on the phone
For a mounted phone streaming for hours, the practical limit is heat
soak: iOS throttles, capture stutters, and at worst the phone shuts down
mid-stream. Watch `ProcessInfo.thermalState` and step down fps → bitrate
→ resolution at `.serious` (restore when it recovers), surface the
adaptation in the app status and the OBS source status, and optionally
respect Low Power Mode. No protocol changes — the app already
reconfigures capture live for lens switches, and the plugin follows
VIDEO_CONFIG. *Moderate effort; the biggest real-world win for long
streams. Recommended next.*

### 2. Full-GPU decode path in the plugin — SHIPPED AS BETA
Implemented as the opt-in **GPU decode pipeline (beta)** in Tools →
LensLink Settings (per-platform interop: D3D11 keyed-mutex shared
textures on Windows, IOSurface/BGRA on macOS, VAAPI dmabuf→EGL on
Linux; automatic per-source fallback to the standard pipeline whenever
interop fails). Remaining before dropping the beta label: field
validation across GPU/driver combinations, and an actual before/after
measurement at 1080p and 4K per the house rule — the beta exists to
gather exactly that.

### 3. Zero-copy encoder output on the phone
The one remaining per-frame copy in the app is the AVCC→Annex B rewrite
into an owned buffer (`VideoEncoder.annexBData`). In-place prefix
rewriting plus retaining the sample buffer until TCP send completion
would eliminate it — but that pins VideoToolbox's buffer pool for up to
12 in-flight frames and can starve the encoder under congestion, which
is why the copy is deliberate today. *Only do this if an Instruments
pass shows the copy mattering, and size the encoder pool explicitly when
you do.*

### Not planned — revisit when the world changes
- **Stream rotation ("Match phone orientation").** Built and working
  (PR #33's original scope, preserved in that branch's history): the
  capture pipeline rotates buffers, the encoder rebuilds with swapped
  dimensions on aspect flips, and OBS follows the bitstream — real
  portrait video for portrait rigs. Shelved by choice, not difficulty:
  it adds a setup-UI toggle few people would use, and a nudged mounted
  phone resizing its OBS source is a support trap. The shipped behavior
  is UI-follows-rotation with a steady sensor-native landscape stream.
  Revisit if users actually ask for portrait streaming.
- **AV1.** Gated on Apple shipping AV1 *hardware encode* (current Apple
  silicon has decode only, and software AV1 encode on a phone is a
  battery fire). When that changes, the plumbing is cheap: a `VideoCodec`
  case + capability probe in the app, an `AV_CODEC_ID_AV1` mapping in the
  plugin — the decoder path is already codec-agnostic with GPU/software
  fallback. Until then HEVC is the right codec for a LAN/USB link.
- **Micro-optimizations** (poll cadences, timesync/ping rates, web-panel
  polling, recv-buffer tuning): all measured or bounded at ≪1% — not
  worth the review risk.

## Feature enhancements (recommended)

Ranked by how much they'd matter to a typical streamer, weighed against
effort. These extend features that already exist. Items 2–8 came out of
a 2026-07 survey of comparable products (Camo, DroidCam, Elgato Camera
Hub/EpocCam, Continuity Camera, iVCam/Iriun, NDI HX Camera/Larix).

### 1. Optional pairing & encryption — close the trusted-LAN caveat
The README honestly says the stream is unencrypted and intended for
trusted networks, and remote start raises the stakes (any LAN peer could
send `start_stream`). A one-time pairing (PIN shown in the app, entered
in OBS) yielding a stored token, carried in the plugin's first packet
and required for CONTROL — plus optional TLS via `NWProtocolTLS` with a
pinned self-signed cert — would make the app safe on shared networks
(dorms, offices, event venues). Designed as opt-in ("Require pairing"
toggle) to keep the zero-config home path frictionless. *Medium effort;
the right thing to ship before promoting remote start heavily.*

### 2. Tally light on the phone
Pro rigs treat tally as table stakes, and no phone-webcam app has it.
The plugin already tracks per-source visibility (it drives "Disconnect
when this source isn't shown anywhere") and can read program/preview
state from the frontend API; send it to the app as a new CONTROL
command (`{"cmd":"tally","state":"live|preview|idle"}` — old apps
ignore unknown commands, so it's compatible) and surface it on the
Live screen. UI_DESIGN.md §2 needs a tally state added to the status
vocabulary. For multi-phone scenes this removes the "which camera am I
on?" guesswork. *Small effort; the best payoff-per-line in this list.*

### 3. Digital pan/tilt + crop — reframe without touching the rig
Zoom today is the camera's own, always center-locked. An adjustable
crop applied on the phone *before* encoding (Camera Hub's signature
control) lets the operator reframe a mounted phone from the web panel —
and because the full encode bitrate then covers only the cropped
region, it reads sharper than cropping in OBS. New CONTROL command with
a normalized crop rect, STATE carries it back, web panel and Live
screen get a drag-to-frame control. *Medium effort; the biggest
quality-of-life gap for the mounted-phone use case this app is built
around.*

### 4. 10-bit HEVC + HDR (HLG/PQ)
iPhones capture HDR by default; the current 8-bit NV12 pipeline throws
that away, and DroidCam already ships 10-bit HDR transfer. App: 10-bit
capture format + HEVC Main-10 in `VideoEncoder`; protocol: VIDEO_CONFIG
gains colour metadata (primaries/transfer/matrix); plugin: map to
P010 + OBS's HDR colour handling (OBS 28+), including the GPU pipeline's
per-backend texture formats. Ship with a sensible SDR default — Twitch
is still SDR, so HDR mainly pays off for YouTube and local recording.
*Large effort across all three layers; the headline video-quality
differentiator.*

### 5. Apple Log capture (after item 4)
On iPhone 15 Pro and later the camera can capture in Apple Log
(`AVCaptureColorSpace.appleLog`); streamers grade it with a LUT in OBS.
Once the 10-bit path exists this is mostly a capture-format toggle plus
correct colour tagging, gated to devices that report support. Nothing
else in this market offers it. *Small effort once item 4 ships.*

### 6. Voice isolation for the phone mic
The mic features send the raw microphone feed; iOS's built-in
voice-processing pipeline (echo cancellation + noise suppression, the
FaceTime mic sound) is nearly free to enable. An Options toggle in the
Microphone group, applied to the send-phone-mic path. *Small effort;
makes the "phone as wireless mic" feature genuinely usable in untreated
rooms.*

### 7. Manual bitrate cap
Adaptive bitrate already reacts to congestion, but there's no user
ceiling. A cap (Options row, plus a live command so the web panel can
set it) keeps shared/metered networks and multi-phone rigs predictable.
STATE advertises the cap so remote UIs stay in lock-step. *Small
effort.*

### 8. Document the control API — Stream Deck without a plugin
The web panel's endpoints (`/api/state`, `POST /api/control`) are
stable and already scriptable; what's missing is a docs page listing
the commands with curl examples and a Stream Deck "Website"-action /
Bitfocus Companion recipe. That unlocks hardware-button control without
shipping or maintaining a native Stream Deck plugin — revisit a real
plugin only if demand shows up. *Docs-only.*

## How to use this document

Pick an item, open a GitHub issue referencing the section, and prune the
entry here when it ships. If reality disagrees with a ranking (a
profile, a pile of user requests), update the ranking — this file is a
map, not a promise.
