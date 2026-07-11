import Foundation
import Network
import UIKit

/// TCP client that frames and sends protocol packets to the OBS plugin.
final class StreamClient {
    enum State: Equatable {
        case disconnected
        case connecting
        case connected
        case failed(String)
    }

    var onStateChange: ((State) -> Void)?

    private(set) var state: State = .disconnected {
        didSet { onStateChange?(state) }
    }

    private var connection: NWConnection?
    private let queue = DispatchQueue(label: "obscam.network")
    private var pingTimer: DispatchSourceTimer?

    func connect(host: String, port: UInt16) {
        disconnect()

        guard let nwPort = NWEndpoint.Port(rawValue: port) else {
            state = .failed("Invalid port")
            return
        }

        let params = NWParameters.tcp
        if let tcpOptions = params.defaultProtocolStack.transportProtocol as? NWProtocolTCP.Options {
            tcpOptions.noDelay = true
            tcpOptions.connectionTimeout = 5
        }

        let connection = NWConnection(host: NWEndpoint.Host(host), port: nwPort, using: params)
        self.connection = connection
        state = .connecting

        connection.stateUpdateHandler = { [weak self] newState in
            guard let self else { return }
            switch newState {
            case .ready:
                self.state = .connected
                self.sendHello()
                self.startPing()
            case .failed(let error):
                self.state = .failed(error.localizedDescription)
                self.teardown()
            case .cancelled:
                self.state = .disconnected
            default:
                break
            }
        }

        // Drain (and ignore) anything the server might send; also detects EOF.
        receiveLoop(on: connection)
        connection.start(queue: queue)
    }

    func disconnect() {
        teardown()
        state = .disconnected
    }

    private func teardown() {
        pingTimer?.cancel()
        pingTimer = nil
        connection?.cancel()
        connection = nil
    }

    private func receiveLoop(on connection: NWConnection) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 4096) { [weak self, weak connection] _, _, isComplete, error in
            guard let self, let connection, self.connection === connection else { return }
            if isComplete || error != nil {
                self.state = .disconnected
                self.teardown()
                return
            }
            self.receiveLoop(on: connection)
        }
    }

    // MARK: - Sending

    private func send(_ data: Data) {
        connection?.send(content: data, completion: .contentProcessed { [weak self] error in
            if let error {
                self?.state = .failed(error.localizedDescription)
                self?.teardown()
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

    func sendVideoFrame(_ frame: H264Encoder.EncodedFrame) {
        let flags: OBSCProtocol.Flags = frame.isKeyframe ? [.keyframe] : []
        send(OBSCProtocol.packet(type: .video,
                                 flags: flags,
                                 ptsNanoseconds: frame.ptsNanoseconds,
                                 payload: frame.data))
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
