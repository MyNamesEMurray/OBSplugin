import Foundation
import Network

/// Diagnostic: checks whether the screen-broadcast extension's listener is
/// reachable on this device, without involving OBS or USB. usbmuxd delivers
/// USB connections to a local port the same way, so a passing probe means
/// the extension side is healthy and any remaining failure is on the
/// OBS/transport side; a failing probe means the listener never opened.
enum BroadcastProbe {
    /// Whether the broadcast extension actually made it into the installed
    /// app. Sideloading tools commonly strip PlugIns when re-signing, and
    /// iOS's broadcast picker can keep showing a stale entry from an older
    /// install — so "it's in the picker" does not mean it's installed.
    static func installedExtensionDescription() -> String {
        guard let plugins = Bundle.main.builtInPlugInsURL,
              let items = try? FileManager.default.contentsOfDirectory(
                  at: plugins, includingPropertiesForKeys: nil)
        else {
            return "✗ No PlugIns folder — the sideloader stripped the extension"
        }
        let appexes = items.filter { $0.pathExtension == "appex" }
        guard !appexes.isEmpty else {
            return "✗ Extension missing from app bundle — re-sideload with app extensions enabled"
        }
        return "✓ Extension installed: "
            + appexes.map { $0.lastPathComponent }.joined(separator: ", ")
    }

    static func run(completion: @escaping (Bool) -> Void) {
        guard let port = NWEndpoint.Port(rawValue: OBSCProtocol.usbPort) else {
            completion(false)
            return
        }
        let connection = NWConnection(host: "127.0.0.1", port: port, using: .tcp)
        let queue = DispatchQueue(label: "lenslink.probe")
        var finished = false
        func finish(_ ok: Bool) {
            guard !finished else { return }
            finished = true
            connection.cancel()
            DispatchQueue.main.async { completion(ok) }
        }
        connection.stateUpdateHandler = { state in
            switch state {
            case .ready:
                finish(true)
            case .failed, .waiting:
                finish(false)
            default:
                break
            }
        }
        connection.start(queue: queue)
        queue.asyncAfter(deadline: .now() + 3) { finish(false) }
    }
}
