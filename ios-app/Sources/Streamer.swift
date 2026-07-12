import Foundation
import AVFoundation
import Combine
import SwiftUI

/// Glues camera → encoder → network together and exposes state for SwiftUI.
@MainActor
final class Streamer: ObservableObject {
    enum Status: Equatable {
        case idle
        case connecting
        case streaming
        case error(String)

        /// The single canonical status word/phrase used by every surface
        /// (see docs/UI_DESIGN.md §2).
        var displayName: String {
            switch self {
            case .idle: return "Not connected"
            case .connecting: return "Waiting for OBS…"
            case .streaming: return "Live"
            case .error(let message): return message
            }
        }

        /// The status dot/label colour from the shared palette.
        var tint: Color {
            switch self {
            case .idle: return Theme.idleGrey
            case .connecting: return Theme.connectAmber
            case .streaming: return Theme.liveGreen
            case .error: return Theme.errorRed
            }
        }
    }

    // Settings (persisted to UserDefaults)
    @Published var resolution: CameraManager.Resolution {
        didSet { UserDefaults.standard.set(resolution.rawValue, forKey: "resolution") }
    }
    @Published var fps: Int {
        didSet { UserDefaults.standard.set(fps, forKey: "fps") }
    }
    /// The cameras this device actually has (Main / Ultra Wide / …).
    let availableLenses: [CameraManager.Lens]

    @Published var selectedLens: CameraManager.Lens {
        didSet {
            UserDefaults.standard.set(selectedLens.id, forKey: "selectedLens")
            clampCaptureSettings()
            // Live lens switch: reconfigure capture under the running
            // encoder and connection.
            if isStreaming {
                try? camera.configure(lens: selectedLens,
                                      resolution: resolution,
                                      fps: Int32(fps))
                resetCameraControls()
                encoder?.requestKeyframe()
                scheduleStateSend()
            }
        }
    }
    @Published var codec: VideoCodec {
        didSet { UserDefaults.standard.set(codec.rawValue, forKey: "videoCodec") }
    }
    @Published var dimWhileStreaming: Bool {
        didSet { UserDefaults.standard.set(dimWhileStreaming, forKey: "dimWhileStreaming") }
    }
    /// Send phone-mic audio as a lip-sync calibration reference (never
    /// played out — the plugin correlates it against your real mic).
    @Published var sendAudioReference: Bool {
        didSet { UserDefaults.standard.set(sendAudioReference, forKey: "sendAudioReference") }
    }
    @Published var micPermissionDenied = false

    // Live camera controls (also driven remotely via CONTROL packets)
    @Published var zoom: CGFloat = 1 {
        didSet {
            camera.setZoom(zoom)
            scheduleStateSend()
        }
    }
    @Published var exposureBias: Float = 0 {
        didSet {
            camera.setExposureBias(exposureBias)
            scheduleStateSend()
        }
    }
    enum FocusSetting: Equatable {
        case auto
        case locked
    }
    @Published var focusSetting: FocusSetting = .auto {
        didSet {
            applyFocus()
            scheduleStateSend()
        }
    }
    @Published var lensPosition: Float = 0.5 {
        didSet {
            if focusSetting == .locked { applyFocus() }
            scheduleStateSend()
        }
    }
    @Published var torchOn: Bool = false {
        didSet {
            camera.setTorch(torchOn)
            scheduleStateSend()
        }
    }

    /// Debounced push of the control state to the plugin (for its web UI).
    private var stateSendPending = false

    private func scheduleStateSend() {
        guard isStreaming, !stateSendPending else { return }
        stateSendPending = true
        Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 200_000_000)
            guard let self else { return }
            self.stateSendPending = false
            guard self.isStreaming else { return }
            self.client.sendState(self.controlStateSnapshot())
        }
    }

    private func controlStateSnapshot() -> [String: Any] {
        [
            "zoom": Double(zoom),
            "maxZoom": Double(camera.maxZoomFactor),
            "exposureBias": Double(exposureBias),
            "focusMode": focusSetting == .locked ? "locked" : "auto",
            "lensPosition": Double(lensPosition),
            "torch": torchOn,
            "hasTorch": camera.hasTorch,
            "camera": selectedLens.position == .front ? "front" : "back",
            "lens": selectedLens.label,
            "lenses": availableLenses.map { $0.label },
        ]
    }

    private func applyFocus() {
        switch focusSetting {
        case .auto:
            camera.setContinuousAutoFocus()
        case .locked:
            camera.lockFocus(lensPosition: lensPosition)
        }
    }

    private func resetCameraControls() {
        zoom = 1
        exposureBias = 0
        focusSetting = .auto
        torchOn = false
    }

    // State
    @Published private(set) var status: Status = .idle
    @Published private(set) var isStreaming = false
    @Published var cameraPermissionDenied = false

    /// Keeps resolution/fps within what the selected lens supports.
    func clampCaptureSettings() {
        let supported = { (r: CameraManager.Resolution, f: Int) in
            CameraManager.supports(resolution: r, fps: Int32(f),
                                   lens: self.selectedLens)
        }
        if supported(resolution, fps) { return }
        if supported(resolution, 30) {
            fps = 30
            return
        }
        for fallback in [CameraManager.Resolution.hd1080, .hd720] {
            if supported(fallback, fps) {
                resolution = fallback
                return
            }
            if supported(fallback, 30) {
                resolution = fallback
                fps = 30
                return
            }
        }
    }

    let camera = CameraManager()
    private var encoder: VideoEncoder?
    private let client = StreamClient()
    private var audioReference: AudioReference?

    init() {
        let defaults = UserDefaults.standard
        resolution = CameraManager.Resolution(
            rawValue: defaults.string(forKey: "resolution") ?? "") ?? .hd720
        let storedFps = defaults.integer(forKey: "fps")
        fps = storedFps > 0 ? storedFps : 30
        let lenses = CameraManager.availableLenses()
        availableLenses = lenses.isEmpty ? [CameraManager.defaultLens] : lenses
        let savedLensID = defaults.string(forKey: "selectedLens")
        selectedLens = availableLenses.first { $0.id == savedLensID }
            ?? availableLenses[0]
        codec = VideoCodec(rawValue: defaults.string(forKey: "videoCodec") ?? "")
            ?? (VideoEncoder.isSupported(.hevc) ? .hevc : .h264)
        dimWhileStreaming = defaults.object(forKey: "dimWhileStreaming") as? Bool ?? true
        sendAudioReference = defaults.bool(forKey: "sendAudioReference")

        client.onStateChange = { [weak self] state in
            Task { @MainActor [weak self] in
                self?.handleClientState(state)
            }
        }
        client.onControl = { [weak self] json in
            Task { @MainActor [weak self] in
                self?.handleRemoteControl(json)
            }
        }
        client.onFrameDropped = { [weak self] in
            Task { @MainActor [weak self] in
                self?.encoder?.requestKeyframe()
            }
        }
    }

    // MARK: - Remote control (from the OBS plugin / web panel)

    private func handleRemoteControl(_ json: Data) {
        guard isStreaming,
              let object = try? JSONSerialization.jsonObject(with: json),
              let command = object as? [String: Any],
              let cmd = command["cmd"] as? String else { return }

        switch cmd {
        case "zoom":
            if let value = command["value"] as? Double {
                zoom = CGFloat(value)
            }
        case "exposure_bias":
            if let value = command["value"] as? Double {
                exposureBias = Float(value)
            }
        case "focus":
            if let position = command["lensPosition"] as? Double {
                lensPosition = Float(position)
            }
            focusSetting = (command["mode"] as? String) == "locked" ? .locked : .auto
        case "torch":
            if let on = command["on"] as? Bool {
                torchOn = on
            }
        case "flip":
            flipCamera()
        case "selectLens":
            if let label = command["label"] as? String,
               let lens = availableLenses.first(where: { $0.label == label }),
               CameraManager.supports(resolution: resolution,
                                      fps: Int32(fps), lens: lens) {
                selectedLens = lens
            }
        default:
            break
        }
    }

    /// Switches front/back if a lens on the other side supports the
    /// current resolution/fps (the selectedLens observer reconfigures
    /// capture mid-stream).
    func flipCamera() {
        let newPosition: AVCaptureDevice.Position =
            selectedLens.position == .front ? .back : .front
        guard let lens = availableLenses.first(where: {
            $0.position == newPosition &&
            CameraManager.supports(resolution: resolution, fps: Int32(fps),
                                   lens: $0)
        }) else { return }
        selectedLens = lens
    }

    // MARK: - Streaming lifecycle

    func start() async {
        guard !isStreaming else { return }

        guard await CameraManager.requestPermission() else {
            cameraPermissionDenied = true
            status = .error("Camera access denied — enable it in Settings")
            return
        }

        // Fall back to H.264 automatically if this device can't encode HEVC.
        let activeCodec = (codec == .hevc && !VideoEncoder.isSupported(.hevc))
            ? VideoCodec.h264 : codec

        let size = resolution.size
        let encoder = VideoEncoder(codec: activeCodec,
                                   width: size.width, height: size.height,
                                   fps: Int32(fps),
                                   bitrate: resolution.bitrate(for: activeCodec))
        do {
            try camera.configure(lens: selectedLens,
                                 resolution: resolution,
                                 fps: Int32(fps))
            try encoder.start()
        } catch {
            status = .error(error.localizedDescription)
            return
        }

        encoder.onEncodedFrame = { [weak self] frame in
            self?.client.sendVideoFrame(frame)
        }
        camera.onSampleBuffer = { [weak encoder] sampleBuffer in
            encoder?.encode(sampleBuffer)
        }
        self.encoder = encoder

        isStreaming = true
        status = .connecting
        UIApplication.shared.isIdleTimerDisabled = true

        camera.start()
        resetCameraControls()
        startAdaptiveBitrate(target: resolution.bitrate(for: activeCodec))
        client.start(port: OBSCProtocol.usbPort)

        if sendAudioReference {
            await startAudioReference()
        }
    }

    /// Captures phone-mic reference audio for lip-sync calibration.
    private func startAudioReference() async {
        guard await AudioReference.requestPermission() else {
            micPermissionDenied = true
            return
        }
        let reference = AudioReference()
        reference.onPCM = { [weak self] pcm, pts in
            self?.client.sendAudio(pcm, ptsNanoseconds: pts)
        }
        do {
            try reference.start()
            audioReference = reference
        } catch {
            print("Audio reference failed: \(error.localizedDescription)")
        }
    }

    /// Adaptive bitrate: back off quickly when the link drops frames,
    /// recover slowly (+10%/10s of clean streaming) up to the target.
    private var adaptiveTask: Task<Void, Never>?

    private func startAdaptiveBitrate(target: Int) {
        adaptiveTask?.cancel()
        adaptiveTask = Task { [weak self] in
            var current = target
            var stableSeconds = 0
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                guard let self, self.isStreaming else { break }

                let dropped = self.client.takeDroppedFrameCount()
                let sendDelayMs = self.client.takeMaxSendDelayMs()
                if dropped > 0 || sendDelayMs > 200 {
                    stableSeconds = 0
                    let reduced = max(1_000_000, current * 3 / 4)
                    if reduced < current {
                        current = reduced
                        self.encoder?.setBitrate(current)
                        print("Adaptive bitrate: congestion (dropped "
                              + "\(dropped), send delay \(sendDelayMs) ms) "
                              + "-> \(current / 1000) kbps")
                    }
                } else if current < target {
                    stableSeconds += 1
                    if stableSeconds >= 10 {
                        stableSeconds = 0
                        current = min(target, current * 11 / 10)
                        self.encoder?.setBitrate(current)
                    }
                }
            }
        }
    }

    func stop() {
        guard isStreaming else { return }
        isStreaming = false
        status = .idle
        UIApplication.shared.isIdleTimerDisabled = false

        adaptiveTask?.cancel()
        adaptiveTask = nil
        if torchOn {
            torchOn = false
        }
        client.disconnect()
        camera.stop()
        camera.onSampleBuffer = nil
        encoder?.stop()
        encoder = nil
        audioReference?.stop()
        audioReference = nil
    }

    private func handleClientState(_ state: StreamClient.State) {
        guard isStreaming else { return }
        switch state {
        case .connected:
            status = .streaming
            let size = resolution.size
            client.sendVideoConfig(codec: encoder?.codec ?? codec,
                                   width: size.width, height: size.height,
                                   fps: Int32(fps))
            // Fresh connection: make sure OBS gets a decodable frame ASAP,
            // and seed the remote UI with the current control state.
            encoder?.requestKeyframe()
            client.sendState(controlStateSnapshot())
        case .failed(let message):
            // The client keeps its listener alive across connection drops
            // and reports .connecting; .failed means the listener itself
            // died — fatal.
            status = .error(message)
            stopKeepingError()
        case .disconnected, .connecting:
            status = .connecting
        }
    }

    private func stopKeepingError() {
        let currentStatus = status
        stop()
        status = currentStatus
    }
}
