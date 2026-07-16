# LensLink — Performance Notes

Where the cycles and joules actually go, what has been optimized, and how
to measure before optimizing further. The guiding rule: **the codec
hardware and the network are the floor** — the app/plugin code around them
should add as close to zero as possible.

## Where the cost is (by design)

| Stage | Where | Cost | Avoidable? |
|-------|-------|------|------------|
| Camera capture | iPhone ISP/sensor | fixed | No — it's the product |
| H.264/HEVC encode | iPhone media engine (VideoToolbox) | fixed | No (hardware block) |
| Send to socket | iPhone CPU (memcpy + syscall) | ~1 copy/frame | Minimized (see below) |
| Receive + parse | OBS-box CPU | ~0 copies | Already zero-copy into the decoder |
| H.264/HEVC decode | OBS-box GPU (D3D11VA/VideoToolbox/VAAPI) | fixed | Software fallback only when GPU fails |
| GPU→CPU frame download | OBS-box (`av_hwframe_transfer_data`) | 1 copy/frame | No — `obs_source_output_video` needs system memory |
| OBS async frame ingest | OBS-box (`obs_source_output_video`) | 1 copy/frame | No — OBS API copies internally |

## What the code already does (don't regress these)

**iOS app**
- Capture delivers **NV12 video-range** buffers (`420YpCbCr8BiPlanarVideoRange`)
  straight into VideoToolbox — no BGRA, no format conversion, and
  `alwaysDiscardsLateVideoFrames` sheds load instead of queueing it.
- Encoder is tuned for live use: real-time mode, no frame reordering,
  `PrioritizeEncodingSpeedOverQuality`, `MaxFrameDelayCount = 1`.
- One copy per frame out of the encoder (AVCC→Annex B into an owned `Data`,
  sized up-front with `reserveCapacity`). This copy is deliberate:
  wrapping VideoToolbox's block buffer zero-copy would pin encoder-pool
  buffers until TCP send completion — up to 12 in flight — and starve the
  encoder under congestion.
- **Header + payload are sent as two batched writes** (`NWConnection.batch`),
  so the frame payload is *not* memcpy'd a second time just to gain a
  20-byte header prefix (`StreamClient.sendVideoOnQueue`).
- Backpressure drops frames rather than queueing them (bounded memory,
  bounded latency), and adaptive bitrate reacts to send-delay/drops.
- **Dimmed = GPU idle:** the dim overlay disables the preview layer's
  connection, so no preview frames are rendered under the near-black
  cover (`CameraPreviewView.previewEnabled`). The outgoing stream is
  unaffected. On top of the existing OLED-black + brightness drop, this
  is the biggest saver for the "phone mounted behind the monitor" case.
- Idle standby listener costs nothing measurable: no timers, no camera —
  just an accepting socket and 1 Hz timesync replies while OBS is
  connected.

**OBS plugin**
- The receive buffer is parsed **in place**; video packets go to
  libavcodec via pointer (`pkt->data = recv_buffer + offset`) — zero
  copies from socket to decoder. Compaction is one `memmove` of the
  unparsed tail per recv cycle (amortized, not per packet), and the
  buffer shrinks after keyframe spikes so memory isn't pinned.
- Decode is single-threaded with `LOW_DELAY` on purpose: frame threading
  adds a frame of latency per thread and buys nothing for a live stream
  that arrives one access unit at a time.
- One dial-loop thread per source, poll-driven at 200 ms when idle;
  timesync (1 Hz), control forwarding, and diagnostics all piggyback on
  that loop — no extra threads or timers.
- The web panel is a 1 Hz poll of two tiny JSON endpoints on loopback.

## How to measure (before optimizing further)

- **iPhone:** Instruments → *Energy Log* + *Time Profiler* while streaming
  1080p60 and 4K30, once undimmed and once dimmed. The app's own CPU
  should be single-digit percent; the energy split should be dominated by
  camera + display (undimmed) and camera only (dimmed).
- **OBS box:** OBS Studio → Tools → *Stats* (render/encode lag) plus the
  plugin's own log line (`capture->decode latency: avg …`) and, for CPU,
  a profiler over `obs64`/`obs` filtered to the `ios-camera-server`
  thread. That thread should be nearly all `recv`/`memmove`/decoder time.
- The wire protocol's TIMESYNC gives an end-to-end capture→decode latency
  figure continuously — watch it in the source's Status field; ~60 ms
  over USB at 1080p is the expected baseline.

### Pipeline benchmark (built-in, for the GPU-pipeline comparison)

Tools → LensLink Settings → **"Log pipeline benchmark numbers every
5 s"**. Every ~5 s of live streaming, one tagged line lands in the OBS
log:

```
[lenslink][bench] pipeline=standard | 1920x1080 ~60 fps |
  video-path cost/frame: avg 2.84 ms, max 4.10 ms |
  pixel copies: 186.4 MB/s | OBS process CPU: 9.8%
```

- **video-path cost/frame** — CPU time the plugin spends moving each
  decoded frame toward the compositor: GPU download + OBS's frame copy
  on the standard pipeline; texture map/draw prep on the GPU pipeline.
  This is the work the two pipelines do differently, isolated.
- **pixel copies** — decoded video crossing system memory (0 on a
  healthy GPU-pipeline run; nonzero there means the fallback engaged).
- **OBS process CPU** — sampled the same way OBS's own Stats dock does.

While the toggle is on, the plugin also writes one CSV row **per second
of live video** to `bench-<pipeline>-<epoch>.csv` in its config
directory (the OBS log prints the exact path when the file opens).

The before/after recipe:

1. Keep your normal settings, enable the benchmark toggle, and stream
   the scene for ~10 s or more. That's the **before** file.
2. Flip the GPU pipeline setting, restart OBS, and stream the same
   phone/scene/resolution for the same length. That's the **after**
   file.
3. Run `python3 tools/bench-report.py` in a terminal/command prompt.
   With no arguments it lists the benchmark files it finds in the
   plugin's config directory and asks you to pick the before and after
   runs (or pass the two CSV paths directly).
4. It prints and writes `lenslink-bench-report.md` (the comparison
   table — mean/median/p95 per metric with % change) and
   `lenslink-bench-report.html` (the same plus per-second charts),
   ready to paste into the repo.

Sanity check built into the report: "pixels crossing system memory"
must read **0.00** on the GPU-pipeline run — a nonzero value there
means the automatic CPU fallback engaged and the comparison isn't
measuring what you think. Those report numbers are what a performance
claim in this repo should cite.

Any future performance PR should quote at least one of these numbers
before/after.
