# Building the iOS app (OBSCam)

Requirements: macOS with Xcode 15+, an iPhone/iPad running iOS 15+, and a
free or paid Apple Developer account for on-device signing.

The Xcode project is generated with [XcodeGen](https://github.com/yonaskolb/XcodeGen)
so no `.xcodeproj` is checked in:

```bash
brew install xcodegen
cd ios-app
xcodegen generate
open OBSCam.xcodeproj
```

In Xcode:

1. Select the **OBSCam** target → *Signing & Capabilities* → pick your team
   (or set `DEVELOPMENT_TEAM` in `project.yml` before generating).
2. Optionally change the bundle identifier prefix in `project.yml`
   (`com.example` by default).
3. Select your device and **Run**.

On first launch iOS will ask for **camera** and **local network** permission —
both are required.

## Notes

- The app stops streaming when backgrounded (iOS suspends camera capture);
  keep it in the foreground while live. The screen is kept awake
  automatically while streaming.
- Discovery uses a UDP broadcast on port 9978. If your network blocks
  broadcasts (some corporate/guest Wi-Fi), enter the computer's IP manually —
  OBS shows it under Settings, or run `ipconfig` / `ip addr` on the computer.
