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

    // Settings (persisted to UserDefaults)
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
    @Published var useFrontCamera: Bool {
        didSet { UserDefaults.standard.set(useFrontCamera, forKey: "useFrontCamera") }
    }

    // State
    @Published private(set) var status: Status = .idle
    @Published private(set) var isStreaming = false
    @Published var discoveredServers: [DiscoveryClient.Server] = []
    @Published var cameraPermissionDenied = false

    let camera = CameraManager()
    private var encoder: H264Encoder?
    private let client = StreamClient()
    private let discovery = DiscoveryClient()

    init() {
        let defaults = UserDefaults.standard
        host = defaults.string(forKey: "host") ?? ""
        portText = defaults.string(forKey: "port") ?? "\(OBSCProtocol.defaultPort)"
        resolution = CameraManager.Resolution(
            rawValue: defaults.string(forKey: "resolution") ?? "") ?? .hd720
        let storedFps = defaults.integer(forKey: "fps")
        fps = storedFps > 0 ? storedFps : 30
        useFrontCamera = defaults.bool(forKey: "useFrontCamera")

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
        guard !trimmedHost.isEmpty else {
            status = .error("Enter the IP address of the computer running OBS")
            return
        }
        guard let port = UInt16(portText), port >= 1024 else {
            status = .error("Invalid port")
            return
        }

        let size = resolution.size
        let encoder = H264Encoder(width: size.width, height: size.height,
                                  fps: Int32(fps),
                                  bitrate: resolution.defaultBitrate)
        do {
            try camera.configure(position: useFrontCamera ? .front : .back,
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
        client.connect(host: trimmedHost, port: port)
    }

    func stop() {
        guard isStreaming else { return }
        isStreaming = false
        status = .idle
        UIApplication.shared.isIdleTimerDisabled = false

        client.disconnect()
        camera.stop()
        camera.onSampleBuffer = nil
        encoder?.stop()
        encoder = nil
    }

    private var reconnectAttempts = 0
    private static let maxReconnectAttempts = 5

    private func handleClientState(_ state: StreamClient.State) {
        guard isStreaming else { return }
        switch state {
        case .connected:
            reconnectAttempts = 0
            status = .streaming
            let size = resolution.size
            client.sendVideoConfig(width: size.width, height: size.height,
                                   fps: Int32(fps))
            // Fresh connection: make sure OBS gets a decodable frame ASAP.
            encoder?.requestKeyframe()
        case .failed, .disconnected:
            scheduleReconnect(after: state)
        case .connecting:
            status = .connecting
        }
    }

    /// Retry a dropped connection a few times before giving up — keeps a
    /// momentary Wi-Fi hiccup from ending the stream.
    private func scheduleReconnect(after state: StreamClient.State) {
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
        status = .connecting

        let host = self.host.trimmingCharacters(in: .whitespaces)
        let port = UInt16(portText) ?? OBSCProtocol.defaultPort

        Task { [weak self] in
            try? await Task.sleep(nanoseconds: 2_000_000_000)
            guard let self, self.isStreaming else { return }
            self.encoder?.requestKeyframe()
            self.client.connect(host: host, port: port)
        }
    }

    private func stopKeepingError() {
        let currentStatus = status
        stop()
        status = currentStatus
    }
}
