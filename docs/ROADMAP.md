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

### 2. Full-GPU decode path in the plugin
Today every frame does GPU decode → CPU download
(`av_hwframe_transfer_data`) → OBS's async-frame copy → GPU upload:
~2 GB/s of avoidable memory traffic at 4K60. Keeping frames on the GPU
means a synchronous source with per-platform texture interop
(D3D11VA→D3D11 on Windows, VideoToolbox→Metal/GL on macOS,
VAAPI→GL/Vulkan on Linux), a software fallback that keeps working, and a
rethink of the timestamp/lip-sync path (async sources get timestamp
handling from OBS; a sync source does its own). *Large effort, largest
remaining win — but only measurable at high resolutions. Profile a real
1080p and 4K setup first; don't start this on vibes.*

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
effort. These extend features that already exist.

### 1. Bonjour discovery — stop typing IPs
The app advertises its listener as `_lenslink._tcp` (one line on an
`NWListener`); the plugin browses and shows discovered phones by name in
the source properties, the way the USB device picker already works.
Kills the most error-prone step of setup ("enter the phone's IP"),
survives DHCP renumbering, and pairs naturally with remote start: add
the source once, and any time the app opens, OBS finds it and starts it.
mDNS libraries exist for all three plugin platforms (or speak the
protocol directly — it's small). *Medium effort, biggest day-one UX win.*

### 2. Optional pairing & encryption — close the trusted-LAN caveat
The README honestly says the stream is unencrypted and intended for
trusted networks, and remote start raises the stakes (any LAN peer could
send `start_stream`). A one-time pairing (PIN shown in the app, entered
in OBS) yielding a stored token, carried in the plugin's first packet
and required for CONTROL — plus optional TLS via `NWProtocolTLS` with a
pinned self-signed cert — would make the app safe on shared networks
(dorms, offices, event venues). Designed as opt-in ("Require pairing"
toggle) to keep the zero-config home path frictionless. *Medium effort;
the right thing to ship before promoting remote start heavily.*

### 3. Phone mic as a real OBS audio source
Today the phone mic is only a lip-sync *reference* (never heard). Many
users — the issue tracker's DroidCam converts especially — expect the
phone to be usable as a wireless mic. The plumbing exists: the screen
path already plays PCM through `obs_source_output_audio` (packet type
10) with A/V-aligned timestamps; extend the camera source with an
"audio from phone" mode (app toggle + plugin playing type-10 audio on
camera connections, echo-cancelled settings left to OBS). Mutually
exclusive with the lip-sync reference in the UI so the two roles stay
clear. *Medium effort, high demand.*

### 4. Manual white balance, ISO & shutter
The live-control set (zoom/exposure-bias/focus/flashlight/lens/flip)
stops short of what a "camera for OBS" wants under studio lighting:
locked white balance (auto-lock + temperature slider), and manual
ISO/shutter for flicker-free exposure. All are straightforward
`AVCaptureDevice` APIs, ride the existing CONTROL/STATE JSON (unknown
commands are already ignored by old apps, so it's compatible), and the
web panel mirrors them like every other control. *Low-medium effort,
natural extension of an existing surface.*

### 5. Remote format switching from OBS
Lens selection is already remote-controllable; resolution / frame rate /
codec are not — walking to the phone to flip 1080p→4K contradicts the
remote-start story. A `set_format` CONTROL command driving the same live
reconfigure path the lens switch uses (`reconfigureLiveCapture`), with
the capability list (which combos the camera supports) included in the
STATE snapshot so OBS/web-panel pickers only offer what works. *Low
effort on the app side; property-UI work on the plugin side.*

### 6. Orientation metadata
A rotated phone streams sideways video today; streamers work around it
with OBS transforms. Capturing device orientation and either rotating
the buffer at the encoder or signaling it in VIDEO_CONFIG (plugin
applies it) would make portrait rigs first-class. Decide once whether
rotation happens on the phone (simpler for OBS, costs a transform on
device) or in the plugin (free with the GPU render path in Performance
#2). *Medium effort; worth bundling with Performance #2 if that lands.*

## How to use this document

Pick an item, open a GitHub issue referencing the section, and prune the
entry here when it ships. If reality disagrees with a ranking (a
profile, a pile of user requests), update the ranking — this file is a
map, not a promise.
