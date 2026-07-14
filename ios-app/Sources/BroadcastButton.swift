import SwiftUI
import ReplayKit

/// The system broadcast picker, restyled: it fills its container and has
/// its default glyph stripped, so whatever SwiftUI draws underneath acts
/// as the button face. iOS only starts a broadcast from a real tap on
/// RPSystemBroadcastPickerView — it can't be triggered programmatically —
/// so overlaying the (visually emptied) picker over our own styled row is
/// the way to make it look like every other button in the app.
struct BroadcastPickerOverlay: UIViewRepresentable {
    /// The extension's bundle id is the app's + ".broadcast" (project.yml).
    /// Derived at runtime, NOT hardcoded: sideloading tools (Sideloadly,
    /// AltStore) re-sign with a remapped bundle id, and a stale hardcoded
    /// id makes the picker fall back to listing every broadcast provider
    /// on the phone instead of pre-selecting ours.
    static var extensionBundleID: String {
        (Bundle.main.bundleIdentifier ?? "com.exaltedpixels.LensLinkCamera")
            + ".broadcast"
    }

    func makeUIView(context: Context) -> RPSystemBroadcastPickerView {
        let picker = RPSystemBroadcastPickerView(frame: .zero)
        picker.preferredExtension = Self.extensionBundleID
        // System audio only (see the extension); no mic toggle.
        picker.showsMicrophoneButton = false
        picker.accessibilityLabel = "Start screen broadcast"
        Self.stripChrome(picker)
        return picker
    }

    func updateUIView(_ picker: RPSystemBroadcastPickerView, context: Context) {
        // Re-strip on updates: the system can rebuild the button's imagery
        // (e.g. on appearance changes).
        Self.stripChrome(picker)
    }

    /// Hides the picker's built-in icon and stretches its internal button
    /// to the full bounds. Only the *image* is hidden — dropping the whole
    /// view's alpha below 0.01 would make UIKit skip it in hit-testing.
    private static func stripChrome(_ picker: RPSystemBroadcastPickerView) {
        for case let button as UIButton in picker.subviews {
            button.setImage(nil, for: .normal)
            button.imageView?.alpha = 0
            button.frame = picker.bounds
            button.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        }
    }
}
