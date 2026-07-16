# LensLink — notes for Claude Code sessions

Two deliverables share this repo: an OBS Studio plugin (C, `obs-plugin/`)
and an iPhone/iPad app (SwiftUI, `ios-app/`), talking over the wire
protocol in `docs/PROTOCOL.md`. Architecture and build/release setup:
`docs/DEVELOPMENT.md`. UI language for all surfaces: `docs/UI_DESIGN.md`.

## Verifying changes without a Mac

This environment is Linux, so the iOS app cannot be compiled here (SwiftUI
and the other Apple frameworks only exist in Xcode on macOS). The real
compile check is the CI build, which runs on pull requests only — not on
branch pushes.

After editing any Swift file, run:

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

The plugin side compiles here normally with cmake + a C compiler if you
need to check it (`obs-plugin/BUILDING.md`), though CI covers all three
OS targets on PRs.
