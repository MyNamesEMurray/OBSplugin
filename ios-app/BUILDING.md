# Building the iOS app (LensLink)

Requirements: macOS with Xcode 14.2 or newer, an iPhone/iPad running iOS
15+, and a free or paid Apple Developer account for on-device signing.

> **Older Macs / Xcode 14.x:** the latest XcodeGen can emit a project format
> only Xcode 16+ opens. If Xcode complains about a "future Xcode project
> file format", generate with XcodeGen 2.38.0 instead:
>
> ```bash
> curl -LO https://github.com/yonaskolb/XcodeGen/releases/download/2.38.0/xcodegen.zip
> unzip xcodegen.zip && cd xcodegen && sudo ./install.sh
> ```
>
> Also note Xcode 14.2 deploys to devices up to iOS 16.2. For phones on a
> newer iOS you need matching DeviceSupport files (e.g. from
> github.com/filsv/iOSDeviceSupport) copied into
> `/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/DeviceSupport/`.

The Xcode project is generated with [XcodeGen](https://github.com/yonaskolb/XcodeGen)
so no `.xcodeproj` is checked in:

```bash
brew install xcodegen
cd ios-app
xcodegen generate
open LensLink.xcodeproj
```

In Xcode:

1. Select the **LensLink** target → *Signing & Capabilities* → pick your team
   (or set `DEVELOPMENT_TEAM` in `project.yml` before generating).
2. Optionally change the bundle identifier prefix in `project.yml`
   (`com.exaltedpixels` by default — you'll need your own prefix, since
   that one is registered to the project's Apple account).
3. Select your device and **Run**.

On first launch iOS will ask for **camera** and **local network** permission —
both are required.

## Notes

- **No Mac handy?** `ios-app/syntax-check.sh` parse-checks every Swift
  source on Linux (it installs a Swift toolchain on first run). Syntax
  only — SwiftUI/UIKit don't exist outside Xcode, so the full compile
  still needs macOS (CI does it on every pull request).
- The app stops streaming when backgrounded (iOS suspends camera capture);
  keep it in the foreground while live. The screen is kept awake
  automatically while streaming.
- Discovery works via Bonjour/mDNS: the app advertises `_lenslink._tcp`
  on its TCP listener (port 9979) and the plugin's Phone dropdown browses
  for it. If your network blocks mDNS (some corporate/guest Wi-Fi), enter
  the phone's IP — shown on the app's main screen — into the source's
  Phone field manually.
