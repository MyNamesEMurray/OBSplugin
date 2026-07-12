# OBS iOS Camera

Use your iPhone or iPad as a high-quality camera **directly inside OBS
Studio** — over Wi-Fi or a USB cable. No virtual-camera drivers, no RTMP
server, no monthly subscription.

It comes in two parts: an **OBS plugin** (adds an "iOS Camera" source) and
the **OBSCam iPhone/iPad app**. You install both, then OBS connects to your
phone.

## What you need

- **OBS Studio 32** or newer, on Windows, macOS, or Linux.
- An **iPhone or iPad on iOS/iPadOS 15 or later**.
- For **USB**: iTunes installed on Windows (it provides Apple's device
  driver); nothing extra on macOS.
- To install the app: a Mac with Xcode, **or** a Windows/Mac sideloading
  tool such as [Sideloadly](https://sideloadly.io) and a free Apple ID.

## Install

**1. The OBS plugin.** Download the build for your system from the
[Releases](../../releases) page:

- **Windows** — unzip `ios-camera-source-windows-x64.zip` into
  `C:\Program Files\obs-studio\` (merge the `obs-plugins` and `data`
  folders), then restart OBS.
- **Linux** — extract `ios-camera-source-linux-x86_64.tar.gz` into
  `~/.config/obs-studio/plugins/`.
- **macOS / building yourself** — see
  [`obs-plugin/BUILDING.md`](obs-plugin/BUILDING.md).

**2. The OBSCam app.** Download `OBSCam-unsigned.ipa` from
[Releases](../../releases) and install it with
[Sideloadly](https://sideloadly.io) using a free Apple ID (the app then
needs re-signing about once a week — a limitation of free Apple accounts).
Or build it yourself with Xcode: [`ios-app/BUILDING.md`](ios-app/BUILDING.md).

## Connect

1. On the phone, open **OBSCam**, choose your camera/resolution/frame rate,
   and tap **Start**. The app shows the phone's IP address.
2. In OBS, add a source: **Sources → + → iOS Camera**.
3. Point it at your phone:
   - **Wi-Fi** — enter the IP the app shows as the **Phone IP**. (Phone and
     computer must be on the same network.)
   - **USB** — connect the cable, set **Connection → USB cable**. No
     network needed, and the phone charges while streaming.

The video appears within a second or two. Closing or backgrounding the app
blanks the source.

## Features

- **Wi-Fi or USB.** USB needs no network at all, is lower-latency, and
  charges the phone as you stream.
- **Up to 4K at 60 fps**, in **H.264 or HEVC** (HEVC looks the same at
  ~40% less data). The app only offers resolution/frame-rate combinations
  your specific camera supports.
- **Pick any lens** — Main, Ultra Wide, Telephoto, or Front — switchable
  live while you stream.
- **Multiple cameras.** Add one "iOS Camera" source per phone. On USB you
  can pin a source to a specific device so the same phone always maps to
  the same source.
- **Live camera controls**, both on the phone (full-screen view with pinch
  zoom, tap-to-focus, exposure, focus lock, flashlight, camera flip) and
  **from your computer** via a browser panel at
  `http://localhost:9980` (zoom / exposure / focus / flashlight / flip).
- **Automatic lip sync.** The plugin measures the camera's latency and can
  automatically line up a separate microphone with the video — no guessing
  at delay values. (See "Lip sync" below.)
- **Smooth on weak Wi-Fi.** If the connection congests, the app lowers
  quality briefly and recovers, instead of piling up latency.
- **Battery saver.** While streaming, the phone screen dims after a few
  seconds; tap to wake it.

## Lip sync

Streamers usually use their own microphone, not the phone's. The plugin can
line that mic up with the video automatically:

1. In the iOS Camera source properties, pick your microphone under
   **Lip-sync audio source** and enable **Auto-calibrate**.
2. In the app, turn on **Auto lip-sync reference**.

The phone then sends its microphone purely as a *timing reference* (never
heard on stream); the plugin compares it with your real mic to measure the
exact offset and correct it. If your mic is *slower* than the video (some
USB audio interfaces are), enable **Auto video delay** and it delays the
video to match instead.

## Latency

Over USB you can expect roughly **60 ms** from the camera to OBS at 1080p
or 4K; Wi-Fi is a little higher and more variable. The plugin shows a live
latency figure in the source's Status field and the OBS log.

A few things already minimize it: GPU-accelerated decoding, low-latency
decode settings, and dropping (rather than queuing) frames when the link
stalls. **USB is the most consistent** and immune to Wi-Fi hiccups. For the
lowest possible delay, use USB and good lighting (brighter scenes let the
camera expose faster).

## Tips & troubleshooting

- **USB device not found (Windows):** make sure iTunes is installed — it
  provides the driver the plugin needs — and tap **Trust** on the phone
  when prompted.
- **The app stops working after a week:** free Apple IDs expire sideloaded
  apps every 7 days. Re-install it with Sideloadly to refresh; your
  settings are kept.
- **Two sources, same phone:** one phone can feed one source. A second
  source aimed at the same device will say it's already in use — point it
  at a different phone or remove it.
- **Wi-Fi latency spikes when zoomed in:** heavy digital zoom is harder to
  compress; use the Telephoto lens for clean magnification, or switch to
  USB.
- The stream is unencrypted on your local network — intended for trusted
  home/studio networks.

## Contributing

Architecture, the wire protocol, and the build/release setup are documented
in [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md).
