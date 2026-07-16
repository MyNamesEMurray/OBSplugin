import Foundation
import Network

/// Forces iOS to present the Local Network permission dialog.
///
/// Advertising a Bonjour service (`NWListener.service`) can be *denied*
/// without iOS ever showing the permission prompt — no TCC record is
/// created, so no "Local Network" toggle appears in Settings either, and
/// the user has no way to grant access. Browsing, unlike advertising,
/// reliably registers the attempt: it presents the dialog when the state
/// is undetermined, and even a denial creates the Settings toggle.
///
/// So when the listener lands in its advertise-denied fallback, we run
/// one throwaway browse for our own service type (declared in
/// `NSBonjourServices`) and report how it resolved. The browse finds
/// nothing useful — it exists purely to make iOS ask the question.
final class LocalNetworkPrompt {
    private var browser: NWBrowser?
    private var completion: ((Bool) -> Void)?

    /// Starts the one-shot browse. Calls `completion(granted)` on the
    /// main queue once the authorization resolves; safe to call again
    /// only after that. `granted == true` means the caller can re-listen
    /// with advertising and expect it to stick.
    func trigger(completion: @escaping (Bool) -> Void) {
        guard browser == nil else { return }
        self.completion = completion

        let browser = NWBrowser(
            for: .bonjour(type: "_lenslink._tcp", domain: nil),
            using: NWParameters())
        self.browser = browser

        browser.stateUpdateHandler = { [weak self] state in
            switch state {
            case .ready:
                // Authorized (the user allowed the prompt, or access was
                // already granted).
                self?.finish(granted: true)
            case .waiting(let error):
                // Denied: the browse parks in .waiting on the NoAuth
                // error and would sit there forever.
                print("Local network browse waiting: \(error)")
                self?.finish(granted: false)
            case .failed(let error):
                print("Local network browse failed: \(error)")
                self?.finish(granted: false)
            default:
                break
            }
        }
        browser.start(queue: .main)
    }

    private func finish(granted: Bool) {
        browser?.cancel()
        browser = nil
        let completion = self.completion
        self.completion = nil
        completion?(granted)
    }
}
