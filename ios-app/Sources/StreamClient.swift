import Foundation
import Network
import UIKit

/// Sends protocol packets to the OBS plugin over one of two transports:
/// - dial: TCP client to the plugin's listener (Wi-Fi mode)
/// - listen: TCP server the plugin dials through usbmuxd (USB mode)
final class StreamClient {
    enum State: Equatable {
        case disconnected
        case connecting
        case connected
        case failed(String)
    }

    enum TransportMode {
        case dial(host: String, port: UInt16)
        case listen(port: UInt16)
    }

    var onStateChange: ((State) -> Void)?

    private(set) var state: State = .disconnected {
        didSet { onStateChange?(state) }
    }

    private var connection: NWConnection?
    private var listener: NWListener?
    private let queue = DispatchQueue(label: "obscam.network")
    private var pingTimer: DispatchSourceTimer?

    /// Video frames queued into the connection but not yet processed.
    /// Guarded by `queue` (all sends and completions run there).
    private var inFlightFrames = 0
    private static let maxInFlightFrames = 60

    // MARK: - Lifecycle

    func start(_ mode: TransportMode) {
        // Clean up any previous transport *silently* — announcing a state
        // change here would make every (re)connect look like a fresh drop
        // to the Streamer, which then schedules another reconnect: an
        // endless 2-second connect/teardown loop.
        teardownConnection()
        teardownListener()

        switch mode {
        case .dial(let host, let port):
            dial(host: host, port: port)
        case .listen(let port):
            listen(on: port)
        }
    }

    func disconnect() {
        teardownConnection()
        teardownListener()
        state = .disconnected
    }

    private func teardownConnection() {
        pingTimer?.cancel()
        pingTimer = nil
        connection?.cancel()
        connection = nil
        queue.async { [weak self] in
            self?.inFlightFrames = 0
        }
    }

    private func teardownListener() {
        listener?.cancel()
        listener = nil
    }

    // MARK: - Transports

    private static func tcpParameters() -> NWParameters {
        let params = NWParameters.tcp
        if let tcpOptions = params.defaultProtocolStack.transportProtocol as? NWProtocolTCP.Options {
            tcpOptions.noDelay = true
            tcpOptions.connectionTimeout = 5
        }
        return params
    }

    private func dial(host: String, port: UInt16) {
        guard let nwPort = NWEndpoint.Port(rawValue: port) else {
            state = .failed("Invalid port")
            return
        }

        let connection = NWConnection(host: NWEndpoint.Host(host),
                                      port: nwPort,
                                      using: Self.tcpParameters())
        state = .connecting
        adopt(connection)
    }

    private func listen(on port: UInt16) {
        guard let nwPort = NWEndpoint.Port(rawValue: port),
              let listener = try? NWListener(using: Self.tcpParameters(), on: nwPort) else {
            state = .failed("Could not open USB listener")
            return
        }

        self.listener = listener
        state = .connecting

        listener.newConnectionHandler = { [weak self, weak listener] newConnection in
            guard let self, let listener, self.listener === listener else {
                newConnection.cancel()
                return
            }
            // Newest connection wins (e.g. OBS restarted).
            self.teardownConnection()
            self.adopt(newConnection)
        }
        listener.stateUpdateHandler = { [weak self, weak listener] newState in
            guard let self, let listener, self.listener === listener else { return }
            if case .failed(let error) = newState {
                self.state = .failed(error.localizedDescription)
                self.teardownListener()
                self.teardownConnection()
            }
        }

        listener.start(queue: queue)
    }

    /// Wires up state handling for a connection (dialed or accepted) and
    /// starts it.
    private func adopt(_ connection: NWConnection) {
        self.connection = connection

        connection.stateUpdateHandler = { [weak self, weak connection] newState in
            guard let self, let connection,
                  self.connection === connection else { return }
            switch newState {
            case .ready:
                self.state = .connected
                self.sendHello()
                self.startPing()
                // Drain (and ignore) anything the server might send; also
                // detects EOF. Must only start once the connection is ready.
                self.receiveLoop(on: connection)
            case .waiting(let error):
                // Keep trying (e.g. transient route/permission delays), but
                // surface why we're stuck.
                self.state = .connecting
                print("StreamClient waiting: \(error)")
            case .failed(let error):
                self.connectionEnded(failure: error.localizedDescription)
            case .cancelled:
                break
            default:
                break
            }
        }

        connection.start(queue: queue)
    }

    /// The active connection dropped. In listen mode we stay up and wait
    /// for the peer to dial back; in dial mode we report the drop.
    private func connectionEnded(failure: String?) {
        teardownConnection()
        if listener != nil {
            state = .connecting
        } else if let failure {
            state = .failed(failure)
        } else {
            state = .disconnected
        }
    }

    private func receiveLoop(on connection: NWConnection) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 4096) { [weak self, weak connection] _, _, isComplete, error in
            guard let self, let connection, self.connection === connection else { return }
            if isComplete || error != nil {
                self.connectionEnded(failure: nil)
                return
            }
            self.receiveLoop(on: connection)
        }
    }

    // MARK: - Sending

    /// True when `error` just means the connection was cancelled by us —
    /// expected during teardown/reconnect, never a real failure.
    private static func isCancellation(_ error: NWError) -> Bool {
        if case .posix(let code) = error, code == .ECANCELED {
            return true
        }
        return false
    }

    private func send(_ data: Data, isVideoFrame: Bool = false) {
        // All connection access and counter updates happen on `queue`;
        // callers may be on the main thread or the encoder thread.
        queue.async { [weak self] in
            guard let self else { return }
            self.sendOnQueue(data, isVideoFrame: isVideoFrame)
        }
    }

    private func sendOnQueue(_ data: Data, isVideoFrame: Bool) {
        guard let connection else { return }
        connection.send(content: data, completion: .contentProcessed { [weak self, weak connection] error in
            guard let self else { return }
            if isVideoFrame && self.inFlightFrames > 0 {
                self.inFlightFrames -= 1
            }
            // Ignore results from connections we've already abandoned, and
            // our own cancellations — reacting to those poisons the next
            // connection attempt.
            guard let connection, self.connection === connection else { return }
            if let error, !Self.isCancellation(error) {
                self.connectionEnded(failure: error.localizedDescription)
            }
        })
    }

    private func sendHello() {
        let hello: [String: Any] = [
            "name": UIDevice.current.name,
            "app": "OBSCam",
            "protocol": Int(OBSCProtocol.version),
        ]
        guard let payload = try? JSONSerialization.data(withJSONObject: hello) else { return }
        send(OBSCProtocol.packet(type: .hello, payload: payload))
    }

    func sendVideoConfig(width: Int32, height: Int32, fps: Int32) {
        let config: [String: Any] = [
            "codec": "h264",
            "width": Int(width),
            "height": Int(height),
            "fps": Int(fps),
        ]
        guard let payload = try? JSONSerialization.data(withJSONObject: config) else { return }
        send(OBSCProtocol.packet(type: .videoConfig, payload: payload))
    }

    /// Called from the encoder's output thread; hops to the network queue
    /// so the in-flight counter stays consistent. If the network can't keep
    /// up, non-keyframes are dropped rather than queued without bound.
    func sendVideoFrame(_ frame: H264Encoder.EncodedFrame) {
        queue.async { [weak self] in
            guard let self, self.connection != nil else { return }
            if self.inFlightFrames >= Self.maxInFlightFrames && !frame.isKeyframe {
                return
            }
            self.inFlightFrames += 1
            let flags: OBSCProtocol.Flags = frame.isKeyframe ? [.keyframe] : []
            self.sendOnQueue(OBSCProtocol.packet(type: .video,
                                                 flags: flags,
                                                 ptsNanoseconds: frame.ptsNanoseconds,
                                                 payload: frame.data),
                             isVideoFrame: true)
        }
    }

    private func startPing() {
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + 2, repeating: 2)
        timer.setEventHandler { [weak self] in
            self?.send(OBSCProtocol.packet(type: .ping, payload: Data()))
        }
        timer.resume()
        pingTimer = timer
    }
}
