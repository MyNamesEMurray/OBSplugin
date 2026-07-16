# Contributing

Thanks for helping build LensLink! This page is the short version — the
deep dives live in [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md)
(architecture, repo layout, CI, release automation),
[`docs/PROTOCOL.md`](docs/PROTOCOL.md) (the wire protocol),
[`docs/PERFORMANCE.md`](docs/PERFORMANCE.md) (performance ground rules),
and [`docs/UI_DESIGN.md`](docs/UI_DESIGN.md) (the design system all UI
surfaces follow).

## Building

- **OBS plugin** (C, CMake): [`obs-plugin/BUILDING.md`](obs-plugin/BUILDING.md)
- **iOS app** (SwiftUI, XcodeGen): [`ios-app/BUILDING.md`](ios-app/BUILDING.md)
- No Mac? `ios-app/syntax-check.sh` parse-checks the Swift sources on
  Linux; the full app compile runs in CI on macOS.

## Pull requests

- Keep a PR to one topic. CI builds only what the PR touches — the plugin
  on Ubuntu and Windows with `-Wall -Wextra -Werror`, the app for the iOS
  Simulator on macOS — and PRs merge automatically once the required
  checks pass.
- Match the style around you: the C code follows OBS conventions (tabs),
  Swift uses 4 spaces. An `.editorconfig` covers the basics.
- Say how you tested. Much of LensLink is a live A/V path CI can't
  exercise — note the OS, OBS version, device, and connection (Wi-Fi/USB)
  you tried, or that the change is doc/build-only.
- Changes to the wire protocol must update `docs/PROTOCOL.md` and stay
  compatible with the version negotiation described there.
- By contributing you agree your work is licensed under the project's
  license, [GPL-2.0-or-later](LICENSE).

## Releases

Merging to `main` releases automatically when `obs-plugin/` or `ios-app/`
changed: a patch bump by default, or the bump named by a
`Release-Bump: minor` / `Release-Bump: major` trailer line in a commit of
the PR (`Release-Skip: true` suppresses it). Details in
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md#releases).

## Bugs and ideas

Please use the issue forms — the bug form asks for the handful of facts
(versions, connection type, OBS log) that make phone↔plugin problems
diagnosable from one report. Check
[`docs/ROADMAP.md`](docs/ROADMAP.md) before filing a feature request; it
may already be planned.
