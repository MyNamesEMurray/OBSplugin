#if canImport(AppIntents)
import AppIntents

/// Siri / Shortcuts entry points for remote start (iOS 16+; iOS 15 can use
/// the lenslink://start URL in a Shortcuts "Open URL" action instead).
///
/// The camera can only capture while the app is foreground, so the start
/// intent opens the app — "Hey Siri, start streaming with LensLink" wakes
/// the phone straight into a running stream, no touch needed.
@available(iOS 16.0, *)
struct StartCameraStreamIntent: AppIntent {
    static var title: LocalizedStringResource = "Start Camera Stream"
    static var description = IntentDescription(
        "Opens LensLink and starts streaming the camera to OBS.")
    static var openAppWhenRun = true

    @MainActor
    func perform() async throws -> some IntentResult {
        await Streamer.shared.start()
        return .result()
    }
}

@available(iOS 16.0, *)
struct StopCameraStreamIntent: AppIntent {
    static var title: LocalizedStringResource = "Stop Camera Stream"
    static var description = IntentDescription(
        "Stops the LensLink camera stream.")

    @MainActor
    func perform() async throws -> some IntentResult {
        Streamer.shared.stop()
        return .result()
    }
}

/// Pre-registered Siri phrases — no Shortcuts setup required.
///
/// Deliberately the iOS 16.0 `AppShortcut(intent:phrases:)` initializer,
/// not the 16.4+ one with shortTitle/systemImageName: gating the provider
/// at 16.4 left a 16.0–16.3 dead zone where no phrases existed at all,
/// and the phrases matter more than the Spotlight tile cosmetics. The
/// deprecation warning is accepted for the wider floor.
///
/// Note: Siri phrases only register when the app keeps its real bundle
/// ID. Sideloading tools that re-sign under a personal team rewrite the
/// bundle ID, which orphans the compiled phrase model — the intents still
/// work from the Shortcuts app, but "Hey Siri…" won't match. TestFlight
/// and App Store installs are unaffected (see README troubleshooting).
@available(iOS 16.0, *)
struct LensLinkShortcuts: AppShortcutsProvider {
    @available(iOS, deprecated: 16.4) // silence just the init deprecation
    static var appShortcuts: [AppShortcut] {
        AppShortcut(
            intent: StartCameraStreamIntent(),
            phrases: [
                "Start streaming with \(.applicationName)",
                "Start the \(.applicationName) camera",
                "Start streaming to OBS with \(.applicationName)",
                "Start \(.applicationName)",
            ])
        AppShortcut(
            intent: StopCameraStreamIntent(),
            phrases: [
                "Stop streaming with \(.applicationName)",
                "Stop the \(.applicationName) camera",
                "Stop \(.applicationName)",
            ])
    }
}
#endif
