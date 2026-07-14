import SwiftUI
import ReplayKit

/// Wraps the system broadcast picker, pre-pointed at our upload extension,
/// so the user can start screen mirroring from inside the app instead of
/// digging through Control Center.
struct BroadcastButton: UIViewRepresentable {
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
        let picker = RPSystemBroadcastPickerView(
            frame: CGRect(x: 0, y: 0, width: 52, height: 52))
        picker.preferredExtension = Self.extensionBundleID
        // System audio only (see the extension); no mic toggle.
        picker.showsMicrophoneButton = false
        return picker
    }

    func updateUIView(_ uiView: RPSystemBroadcastPickerView, context: Context) {}
}
