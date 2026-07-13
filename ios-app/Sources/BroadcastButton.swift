import SwiftUI
import ReplayKit

/// Wraps the system broadcast picker, pre-pointed at our upload extension,
/// so the user can start screen mirroring from inside the app instead of
/// digging through Control Center.
struct BroadcastButton: UIViewRepresentable {
    /// Must match the extension's bundle id in project.yml.
    static let extensionBundleID = "com.exaltedpixels.LensLink.broadcast"

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
