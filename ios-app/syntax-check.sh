#!/usr/bin/env bash
# Syntax-check every Swift source in the app + broadcast extension.
#
# Linux has no SwiftUI/UIKit/AVFoundation, so nothing here can *type*-check
# the app — the macOS CI job on pull requests does that. What this catches,
# in a couple of seconds, is every parse-level error (unbalanced braces,
# malformed closures, bad string interpolation) before a push.
#
# Uses the system swiftc if one exists (macOS with Xcode, or an installed
# Linux toolchain); otherwise downloads the Swift toolchain once into a
# cache directory (~840 MB download, ~40 s) and reuses it for the life of
# the machine/container.
set -euo pipefail
cd "$(dirname "$0")"

SWIFT_VERSION="${SWIFT_VERSION:-6.1}"
SWIFT_PLATFORM="${SWIFT_PLATFORM:-ubuntu2404}"     # download.swift.org path segment
SWIFT_PLATFORM_DIR="${SWIFT_PLATFORM_DIR:-ubuntu24.04}"  # directory inside the tarball

if command -v swiftc >/dev/null 2>&1; then
    SWIFTC=swiftc
else
    CACHE="${SWIFT_TOOLCHAIN_CACHE:-$HOME/.cache/lenslink-swift}"
    SWIFTC="$CACHE/swift-$SWIFT_VERSION-RELEASE-$SWIFT_PLATFORM_DIR/usr/bin/swiftc"
    if [ ! -x "$SWIFTC" ]; then
        echo "swiftc not found — installing Swift $SWIFT_VERSION to $CACHE …" >&2
        mkdir -p "$CACHE"
        curl -fsSL "https://download.swift.org/swift-$SWIFT_VERSION-release/$SWIFT_PLATFORM/swift-$SWIFT_VERSION-RELEASE/swift-$SWIFT_VERSION-RELEASE-$SWIFT_PLATFORM_DIR.tar.gz" \
            | tar xz -C "$CACHE"
    fi
fi

"$SWIFTC" -parse Sources/*.swift BroadcastExtension/*.swift
echo "✓ Swift sources parse clean (syntax only — the full type check runs in CI on macOS)"
