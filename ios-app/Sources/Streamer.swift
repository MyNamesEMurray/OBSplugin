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

        var label: String {
            switch self {
            case .idle: return "Not connected"
            case .connecting: return "Connecting…"
            case .streaming: return "Streaming to OBS"
            case .error(let message): return message
            }
        }
    }

    enum ConnectionMode: String, CaseIterable, Identifiable {
        /// The app listens; OBS connects to the phone — over the LAN
        /// (enter the phone's IP in OBS) or over the USB cable. Recommended.
        case receive
        /// Legacy: the app dials OBS's listener (IP of the computer).
        case dial

        var id: String { rawValue }
        var label: String { self == .receive ? "OBS → iPhone" : "iPhone → OBS" }
    }

    // Settings (persisted to UserDefaults)
    @Published var connectionMode: ConnectionMode {
        didSet { UserDefaults.standard.set(connectionMode.rawValue, forKey: "connectionMode") }
    }
    @Published var host: String {
        didSet { UserDefaults.standard.set(host, forKey: "host") }
    }
    @Published var portText: String {
        didSet { UserDefaults.standard.set(portText, forKey: "port") }
    }
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
    @Published var discoveredServers: [DiscoveryClient.Server] = []
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
    private let discovery = DiscoveryClient()

    init() {
        let defaults = UserDefaults.standard
        connectionMode = ConnectionMode(
            rawValue: defaults.string(forKey: "connectionMode") ?? "") ?? .receive
        host = defaults.string(forKey: "host") ?? ""
        portText = defaults.string(forKey: "port") ?? "\(OBSCProtocol.defaultPort)"
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

        client.onStateChange = { [weak self] state in
            Task { @MainActor [weak self] in
                self?.handleClientState(state)
            }
        }
        discovery.onServersChanged = { [weak self] servers in
            Task { @MainActor [weak self] in
                self?.discoveredServers = servers
            }
        }
        client.onControl = { [weak self] json in
            Task { @MainActor [weak self] in
                self?.handleRemoteControl(json)
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

    // MARK: - Discovery

    func refreshServers() {
        discoveredServers = []
        discovery.start()
    }

    func select(_ server: DiscoveryClient.Server) {
        host = server.host
        portText = "\(server.port)"
    }

    // MARK: - Streaming lifecycle

    func start() async {
        guard !isStreaming else { return }

        guard await CameraManager.requestPermission() else {
            cameraPermissionDenied = true
            status = .error("Camera access denied — enable it in Settings")
            return
        }

        let trimmedHost = host.trimmingCharacters(in: .whitespaces)
        var dialPort: UInt16 = OBSCProtocol.defaultPort
        if connectionMode == .dial {
            guard !trimmedHost.isEmpty else {
                status = .error("Enter the IP address of the computer running OBS")
                return
            }
            guard let port = UInt16(portText), port >= 1024 else {
                status = .error("Invalid port")
                return
            }
            dialPort = port
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

        reconnectAttempts = 0
        isStreaming = true
        status = .connecting
        UIApplication.shared.isIdleTimerDisabled = true

        camera.start()
        resetCameraControls()
        startAdaptiveBitrate(target: resolution.bitrate(for: activeCodec))
        switch connectionMode {
        case .dial:
            client.start(.dial(host: trimmedHost, port: dialPort))
        case .receive:
            client.start(.listen(port: OBSCProtocol.usbPort))
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
    }

    private var reconnectAttempts = 0
    private var reconnectPending = false
    private static let maxReconnectAttempts = 5

    private func handleClientState(_ state: StreamClient.State) {
        guard isStreaming else { return }
        switch state {
        case .connected:
            reconnectAttempts = 0
            reconnectPending = false
            status = .streaming
            let size = resolution.size
            client.sendVideoConfig(codec: encoder?.codec ?? codec,
                                   width: size.width, height: size.height,
                                   fps: Int32(fps))
            // Fresh connection: make sure OBS gets a decodable frame ASAP,
            // and seed the remote UI with the current control state.
            encoder?.requestKeyframe()
            client.sendState(controlStateSnapshot())
        case .failed, .disconnected:
            // In receive mode the client keeps its listener alive and
            // reports .connecting while waiting for OBS to dial back;
            // .failed there means the listener itself died — fatal.
            if connectionMode == .receive {
                if case .failed(let message) = state {
                    status = .error(message)
                    stopKeepingError()
                } else {
                    status = .connecting
                }
            } else {
                scheduleReconnect(after: state)
            }
        case .connecting:
            status = .connecting
        }
    }

    /// Retry a dropped connection a few times before giving up — keeps a
    /// momentary Wi-Fi hiccup from ending the stream.
    private func scheduleReconnect(after state: StreamClient.State) {
        guard !reconnectPending else { return }
        guard reconnectAttempts < Self.maxReconnectAttempts else {
            if case .failed(let message) = state {
                status = .error(message)
            } else {
                status = .error("Disconnected from OBS")
            }
            stopKeepingError()
            return
        }
        reconnectAttempts += 1
        reconnectPending = true
        status = .connecting

        let host = self.host.trimmingCharacters(in: .whitespaces)
        let port = UInt16(portText) ?? OBSCProtocol.defaultPort

        Task { [weak self] in
            try? await Task.sleep(nanoseconds: 2_000_000_000)
            guard let self, self.isStreaming else { return }
            self.reconnectPending = false
            self.encoder?.requestKeyframe()
            self.client.start(.dial(host: host, port: port))
        }
    }

    private func stopKeepingError() {
        let currentStatus = status
        stop()
        status = currentStatus
    }
}
