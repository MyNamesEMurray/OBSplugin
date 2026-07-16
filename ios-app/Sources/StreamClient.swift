import Foundation
import Network
import UIKit

/// TCP server the OBS plugin dials — over the LAN (phone IP) or through
/// usbmuxd (USB cable). Frames and sends protocol packets to the plugin.
final class StreamClient {
    enum State: Equatable {
        case disconnected
        case connecting
        case connected
        case failed(String)
    }

    var onStateChange: ((State) -> Void)?
    /// Remote camera-control command (JSON) received from the plugin.
    var onControl: ((Data) -> Void)?
    /// A video frame was dropped: the reference chain is broken and the
    /// encoder should produce a fresh keyframe ASAP.
    var onFrameDropped: (() -> Void)?

    private(set) var state: State = .disconnected {
        didSet { onStateChange?(state) }
    }

    private var connection: NWConnection?
    private var listener: NWListener?
    private let queue = DispatchQueue(label: "obscam.network")
    private var pingTimer: DispatchSourceTimer?

    /// Video frames queued into the connection but not yet processed.
    /// Guarded by `queue` (all sends and completions run there).
    /// Kept small: a deep queue is invisible latency — better to drop
    /// non-keyframes and stay current.
    private var inFlightFrames = 0
    private static let maxInFlightFrames = 12

    /// Incoming bytes from the plugin (timesync requests), queue-confined.
    private var receiveBuffer = Data()

    // MARK: - Lifecycle
    //
    // Every mutable property (connection, listener, pingTimer, counters,
    // buffers, state) is confined to `queue`: the NW handlers already run
    // there, so the public entry points hop onto it too instead of racing
    // them from the main thread.

    func start(port: UInt16) {
        queue.async { [weak self] in
            guard let self else { return }
            // Clean up any previous transport *silently* — a state change
            // here would look like a fresh drop to the Streamer.
            self.teardownConnection()
            self.teardownListener()
            self.listen(on: port)
        }
    }

    func disconnect() {
        queue.async { [weak self] in
            guard let self else { return }
            self.teardownConnection()
            self.teardownListener()
            self.state = .disconnected
        }
    }

    /// Queue-confined.
    private func teardownConnection() {
        connectionAuthenticated = false
        pingTimer?.cancel()
        pingTimer = nil
        connection?.cancel()
        connection = nil
        inFlightFrames = 0
        waitingForKeyframe = false
        receiveBuffer.removeAll(keepingCapacity: false)
    }

    /// Queue-confined.
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
            // Without keepalive a silently dead link (Wi-Fi drop, no RST)
            // looks "Live" for minutes while sends pile into the kernel.
            tcpOptions.enableKeepalive = true
            tcpOptions.keepaliveIdle = 5
            tcpOptions.keepaliveInterval = 5
            tcpOptions.keepaliveCount = 2
        }
        return params
    }

    // Diagnostics (queue-confined): what the listener actually did — the
    // broadcast extension has no UI or reachable logs on Windows setups,
    // so failure alerts embed this snapshot.
    private var listenerStateDescription = "not started"
    private var acceptedConnections = 0

    // Cumulative send-side counters for the screen-mirror diagnostics
    // heartbeat (distinct from the adaptive-bitrate counters below, which
    // reset every sample). Queue-confined.
    private var diagVideoSent = 0
    private var diagVideoDropped = 0
    private var diagKeyframesSent = 0
    private var diagVideoBytes = 0
    private var diagAudioChunks = 0

    /// One-line health snapshot, safe to call from any thread.
    func debugStatus() -> String {
        queue.sync {
            "listener=\(listenerStateDescription), "
                + "accepted=\(acceptedConnections), state=\(state)"
        }
    }

    /// Cumulative send-path counters, for the in-app health overlay.
    /// Safe to call from any thread (hops to the network queue).
    struct Stats: Equatable {
        var framesSent = 0
        var bytesSent = 0
        var framesDropped = 0
    }

    func statsSnapshot() -> Stats {
        queue.sync {
            Stats(framesSent: diagVideoSent,
                  bytesSent: diagVideoBytes,
                  framesDropped: diagVideoDropped)
        }
    }

    /// One-line send-path snapshot for the diagnostics heartbeat. The
    /// broadcast extension forwards this to the plugin, which logs it, so
    /// the phone's view of the stream shows up in the OBS log too.
    func diagnosticsSnapshot() -> String {
        queue.sync {
            "net sent=\(diagVideoSent) kf=\(diagKeyframesSent) "
                + "drop=\(diagVideoDropped) \(diagVideoBytes / 1024)KiB "
                + "aud=\(diagAudioChunks) inflight=\(inFlightFrames) "
                + "acc=\(acceptedConnections) \(state)"
        }
    }

    /// Sends a short diagnostics line to the plugin (logged to the OBS log).
    func sendDiag(_ text: String) {
        guard let payload = text.data(using: .utf8) else { return }
        send(OBSCProtocol.packet(type: .diag, payload: payload))
    }

    private func listen(on port: UInt16, advertise: Bool = true) {
        guard let nwPort = NWEndpoint.Port(rawValue: port),
              let listener = try? NWListener(using: Self.tcpParameters(), on: nwPort) else {
            listenerStateDescription = "init failed"
            state = .failed("Could not open USB listener")
            return
        }

        // Advertise over Bonjour so the OBS plugin can discover this
        // phone by name instead of a typed IP. The device name matches
        // the HELLO; port comes from the service record's socket.
        if advertise {
            listener.service = NWListener.Service(
                name: UIDevice.current.name, type: "_lenslink._tcp")
        }

        self.listener = listener
        state = .connecting

        listener.newConnectionHandler = { [weak self, weak listener] newConnection in
            guard let self else {
                newConnection.cancel()
                return
            }
            self.acceptedConnections += 1
            guard let listener, self.listener === listener else {
                newConnection.cancel()
                return
            }
            // Newest connection wins (e.g. OBS restarted).
            self.teardownConnection()
            self.adopt(newConnection)
        }
        listener.stateUpdateHandler = { [weak self, weak listener] newState in
            guard let self, let listener, self.listener === listener else { return }
            self.listenerStateDescription = "\(newState)"
            if case .failed(let error) = newState {
                // Bonjour advertising is denied when Local Network
                // permission is off (Settings → Privacy → Local Network;
                // sideloaded builds sometimes need the toggle flipped once)
                // — DNSServiceErr NoAuth, surfaced as a .dns NWError. The
                // advertisement is a convenience; the listener is the
                // product. Retry without it so typed-IP Wi-Fi and USB
                // connects keep working, minus name discovery in OBS.
                if advertise, case .dns = error {
                    print("Bonjour advertise failed (\(error)) — "
                          + "listening without discovery")
                    self.teardownListener()
                    self.listen(on: port, advertise: false)
                    return
                }
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

    /// The active connection dropped; the listener stays up so the
    /// plugin can dial back in. Queue-confined.
    private func connectionEnded(failure: String?) {
        teardownConnection()
        state = listener != nil ? .connecting : .disconnected
    }

    private func receiveLoop(on connection: NWConnection) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self, weak connection] content, _, isComplete, error in
            guard let self, let connection, self.connection === connection else { return }
            if let content {
                self.receiveBuffer.append(content)
                self.processIncoming()
            }
            if isComplete || error != nil {
                self.connectionEnded(failure: nil)
                return
            }
            self.receiveLoop(on: connection)
        }
    }

    /// Parses packets from the plugin. Today that's only timesync
    /// requests, which we answer immediately for latency measurement.
    private func processIncoming() {
        while receiveBuffer.count >= OBSCProtocol.headerSize {
            let bytes = [UInt8](receiveBuffer.prefix(OBSCProtocol.headerSize))
            // Bad magic means the stream is byte-misaligned; discarding
            // the buffer and carrying on would keep parsing garbage
            // headers silently. Only a reconnect re-frames the stream.
            guard Array(bytes[0..<4]) == OBSCProtocol.magic,
                  bytes[4] == OBSCProtocol.version else {
                connectionEnded(failure: "protocol desync")
                return
            }

            let type = bytes[5]
            let pts = bytes[8..<16].reduce(UInt64(0)) { $0 << 8 | UInt64($1) }
            let payloadSize = bytes[16..<20].reduce(UInt32(0)) { $0 << 8 | UInt32($1) }
            guard payloadSize < 1 << 20 else {
                connectionEnded(failure: "oversized packet")
                return
            }

            let total = OBSCProtocol.headerSize + Int(payloadSize)
            guard receiveBuffer.count >= total else { return }
            let payload = receiveBuffer.subdata(
                in: receiveBuffer.startIndex + OBSCProtocol.headerSize
                    ..< receiveBuffer.startIndex + total)
            receiveBuffer.removeFirst(total)

            switch type {
            case OBSCProtocol.PacketType.timesyncReq.rawValue:
                respondToTimesync(pluginClock: pts)
            case OBSCProtocol.PacketType.control.rawValue:
                onControl?(payload)
            default:
                break
            }
        }
    }

    private func respondToTimesync(pluginClock: UInt64) {
        // Same clock domain as the camera's presentation timestamps
        // (mach host time), so the plugin can relate frame pts to it.
        let now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
        var payload = Data()
        withUnsafeBytes(of: pluginClock.bigEndian) { payload.append(contentsOf: $0) }
        sendOnQueue(OBSCProtocol.packet(type: .timesyncResp,
                                        ptsNanoseconds: now,
                                        payload: payload),
                    isVideoFrame: false, bypassAuth: true)
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

    private func send(_ data: Data, isVideoFrame: Bool = false,
                      bypassAuth: Bool = false) {
        // All connection access and counter updates happen on `queue`;
        // callers may be on the main thread or the encoder thread.
        queue.async { [weak self] in
            guard let self else { return }
            self.sendOnQueue(data, isVideoFrame: isVideoFrame,
                             bypassAuth: bypassAuth)
        }
    }

    private func sendOnQueue(_ data: Data, isVideoFrame: Bool,
                             bypassAuth: Bool = false) {
        guard let connection, bypassAuth || authOK() else { return }
        connection.send(content: data,
                        completion: sendCompletion(for: connection,
                                                   isVideoFrame: isVideoFrame))
    }

    /// Queue-confined. Video frames are the bulk of the bandwidth; sending
    /// header and payload as two batched writes spares every frame a full
    /// memcpy into a combined buffer (packet() would copy the whole payload
    /// again just to gain a 20-byte prefix) — and the matching allocation.
    private func sendVideoOnQueue(header: Data, payload: Data) {
        guard let connection, authOK() else { return }
        let completion = sendCompletion(for: connection, isVideoFrame: true)
        connection.batch {
            // The header rides with the payload; errors and accounting are
            // handled once, on the payload's completion.
            connection.send(content: header, completion: .idempotent)
            connection.send(content: payload, completion: completion)
        }
    }

    private func sendCompletion(for connection: NWConnection,
                                isVideoFrame: Bool) -> NWConnection.SendCompletion {
        let enqueuedAt = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
        return .contentProcessed { [weak self, weak connection] error in
            guard let self else { return }
            // Ignore results from connections we've already abandoned, and
            // our own cancellations — reacting to those poisons the next
            // connection attempt. This must come *before* the counter
            // updates: a burst of stale completions right after reconnect
            // would drain the new connection's in-flight count (weakening
            // backpressure) and log bogus send delays that make adaptive
            // bitrate cut the rate on a healthy link.
            guard let connection, self.connection === connection else { return }
            if isVideoFrame {
                if self.inFlightFrames > 0 {
                    self.inFlightFrames -= 1
                }
                let delay = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - enqueuedAt
                if delay > self.maxSendDelayNs {
                    self.maxSendDelayNs = delay
                }
            }
            if let error, !Self.isCancellation(error) {
                self.connectionEnded(failure: error.localizedDescription)
            }
        }
    }

    /// What this connection streams — "camera" (the app) or "screen" (the
    /// broadcast extension). Sent in the HELLO and video config so the
    /// plugin can label the source and skip camera-only behaviour.
    var sourceKind: OBSCProtocol.SourceKind = .camera

    /// Pairing (docs/PROTOCOL.md "Pairing"): when required, HELLO carries
    /// auth:"required" and outbound media/state is held until the Streamer
    /// verifies the plugin's token or completes a PIN pairing. Both flags
    /// queue-confined; `connectionAuthenticated` is per connection.
    private var authRequired = false
    private var connectionAuthenticated = false

    func setAuthRequired(_ on: Bool) {
        queue.async { [weak self] in self?.authRequired = on }
    }

    /// The Streamer verified a paired token (or completed pairing) for
    /// the current connection.
    func markConnectionAuthenticated() {
        queue.async { [weak self] in self?.connectionAuthenticated = true }
    }

    /// True when sends may flow: auth off, or this connection verified.
    func isConnectionAuthenticated() -> Bool {
        queue.sync { !authRequired || connectionAuthenticated }
    }

    /// Queue-confined check used by the send paths.
    private func authOK() -> Bool {
        !authRequired || connectionAuthenticated
    }

    /// Sends an auth result STATE (auth:"ok"/"denied", optionally a fresh
    /// pairedToken) — the one state packet that must bypass the auth gate.
    func sendAuthResult(_ result: [String: Any]) {
        guard let payload = try? JSONSerialization.data(withJSONObject: result) else { return }
        send(OBSCProtocol.packet(type: .state, payload: payload),
             bypassAuth: true)
    }

    /// Standby (remote start): the app is reachable but the camera isn't
    /// running yet. Advertised in the HELLO so the plugin can offer (or
    /// auto-send) a "start_stream" control command. Queue-confined.
    private var standbyMode = false

    func setStandby(_ on: Bool) {
        queue.async { [weak self] in
            guard let self, self.standbyMode != on else { return }
            self.standbyMode = on
            // Already connected (e.g. OBS just started the stream over the
            // standby connection): re-announce so the plugin flips its
            // standby state immediately instead of waiting for the video
            // config.
            if self.state == .connected {
                self.sendHello()
            }
        }
    }

    private func sendHello() {
        var hello: [String: Any] = [
            "name": UIDevice.current.name,
            "app": "LensLink",
            "protocol": Int(OBSCProtocol.version),
            "kind": sourceKind.rawValue,
        ]
        if standbyMode {
            hello["standby"] = true
        }
        if authRequired {
            hello["auth"] = "required"
        }
        guard let payload = try? JSONSerialization.data(withJSONObject: hello) else { return }
        send(OBSCProtocol.packet(type: .hello, payload: payload),
             bypassAuth: true)
    }

    func sendVideoConfig(codec: VideoCodec, width: Int32, height: Int32, fps: Int32) {
        let config: [String: Any] = [
            "codec": codec.rawValue,
            "width": Int(width),
            "height": Int(height),
            "fps": Int(fps),
            "kind": sourceKind.rawValue,
        ]
        guard let payload = try? JSONSerialization.data(withJSONObject: config) else { return }
        send(OBSCProtocol.packet(type: .videoConfig, payload: payload))
    }

    /// Sends a chunk of screen-mirror system audio (48 kHz stereo S16LE
    /// interleaved), to be played by the plugin as the source's audio.
    func sendScreenAudio(_ pcm: Data, ptsNanoseconds: UInt64) {
        queue.async { [weak self] in
            self?.diagAudioChunks += 1
        }
        send(OBSCProtocol.packet(type: .screenAudio,
                                 ptsNanoseconds: ptsNanoseconds,
                                 payload: pcm))
    }

    /// Reports the app's current camera-control state so remote UIs
    /// (the plugin's web panel) can stay in sync.
    func sendState(_ state: [String: Any]) {
        guard let payload = try? JSONSerialization.data(withJSONObject: state) else { return }
        send(OBSCProtocol.packet(type: .state, payload: payload))
    }

    /// Sends a chunk of reference-audio PCM (lip-sync calibration).
    func sendAudio(_ pcm: Data, ptsNanoseconds: UInt64) {
        send(OBSCProtocol.packet(type: .audio,
                                 ptsNanoseconds: ptsNanoseconds,
                                 payload: pcm))
    }

    /// Called from the encoder's output thread; hops to the network queue
    /// so the in-flight counter stays consistent. If the network can't keep
    /// up, non-keyframes are dropped rather than queued without bound.
    /// Frames dropped due to backpressure since the last check — the
    /// congestion signal for adaptive bitrate.
    private var droppedFrames = 0

    /// Worst send-to-accepted delay since the last check. TCP hides
    /// congestion in the kernel socket buffer long before our own queue
    /// fills; the time a send takes to be accepted exposes it.
    private var maxSendDelayNs: UInt64 = 0

    func takeDroppedFrameCount() -> Int {
        queue.sync {
            let count = droppedFrames
            droppedFrames = 0
            return count
        }
    }

    func takeMaxSendDelayMs() -> Int {
        queue.sync {
            let ns = maxSendDelayNs
            maxSendDelayNs = 0
            return Int(ns / 1_000_000)
        }
    }

    /// After a drop, every following frame references a frame the decoder
    /// never received — decoding them produces grey "error concealment"
    /// smear. Skip them until a fresh keyframe restarts the chain.
    private var waitingForKeyframe = false

    func sendVideoFrame(_ frame: VideoEncoder.EncodedFrame) {
        queue.async { [weak self] in
            guard let self, self.connection != nil else { return }
            // The cap applies to keyframes too. Exempting them looks
            // harmless but is an unbounded-memory bug: every drop requests
            // a keyframe, so during a long stall the exempt keyframes
            // would be enqueued into the dead connection without limit.
            if self.inFlightFrames >= Self.maxInFlightFrames {
                self.droppedFrames += 1
                self.diagVideoDropped += 1
                self.waitingForKeyframe = true
                return
            }
            if frame.isKeyframe {
                self.waitingForKeyframe = false
            } else if self.waitingForKeyframe {
                // Broken reference chain: skip this frame, and — now that
                // there's room to actually send one — ask the encoder for
                // the keyframe that restarts the stream.
                self.diagVideoDropped += 1
                self.onFrameDropped?()
                return
            }
            self.inFlightFrames += 1
            self.diagVideoSent += 1
            self.diagVideoBytes += frame.data.count
            if frame.isKeyframe {
                self.diagKeyframesSent += 1
            }
            let flags: OBSCProtocol.Flags = frame.isKeyframe ? [.keyframe] : []
            let header = OBSCProtocol.header(type: .video,
                                             flags: flags,
                                             ptsNanoseconds: frame.ptsNanoseconds,
                                             payloadSize: frame.data.count)
            self.sendVideoOnQueue(header: header, payload: frame.data)
        }
    }

    private func startPing() {
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + 2, repeating: 2)
        timer.setEventHandler { [weak self] in
            self?.send(OBSCProtocol.packet(type: .ping, payload: Data()),
                       bypassAuth: true)
        }
        timer.resume()
        pingTimer = timer
    }
}
