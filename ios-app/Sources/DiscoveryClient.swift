import Foundation
import Darwin

/// Finds OBS instances on the LAN by UDP-broadcasting a probe and collecting
/// "OBSC_HERE:<port>:<name>" replies from the plugin's discovery responder.
final class DiscoveryClient {
    struct Server: Identifiable, Equatable {
        let host: String
        let port: UInt16
        let name: String
        var id: String { "\(host):\(port)" }
    }

    var onServersChanged: (([Server]) -> Void)?

    private let queue = DispatchQueue(label: "obscam.discovery")
    private var socketFD: Int32 = -1
    private var running = false
    private var servers: [Server] = []

    func start() {
        queue.async { [weak self] in
            self?.run()
        }
    }

    func stop() {
        queue.async { [weak self] in
            guard let self else { return }
            self.running = false
            if self.socketFD >= 0 {
                close(self.socketFD)
                self.socketFD = -1
            }
        }
    }

    private func run() {
        guard !running else { return }

        let fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else { return }
        socketFD = fd
        running = true
        servers = []

        var enable: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enable, socklen_t(MemoryLayout<Int32>.size))

        var timeout = timeval(tv_sec: 1, tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))

        let discoveryPort = OBSCProtocol.defaultPort + OBSCProtocol.discoveryPortOffset
        var broadcastAddr = sockaddr_in()
        broadcastAddr.sin_family = sa_family_t(AF_INET)
        broadcastAddr.sin_port = discoveryPort.bigEndian
        broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST

        let probe = Array(OBSCProtocol.discoverRequest.utf8)

        // Probe a few times, listening for replies in between.
        for _ in 0..<5 where running {
            probe.withUnsafeBytes { bytes in
                _ = withUnsafePointer(to: &broadcastAddr) { addr in
                    addr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                        sendto(fd, bytes.baseAddress, bytes.count, 0, sa,
                               socklen_t(MemoryLayout<sockaddr_in>.size))
                    }
                }
            }
            receiveReplies(fd)
        }

        if socketFD >= 0 {
            close(socketFD)
            socketFD = -1
        }
        running = false
    }

    private func receiveReplies(_ fd: Int32) {
        var buffer = [UInt8](repeating: 0, count: 512)
        var from = sockaddr_in()
        var fromLen = socklen_t(MemoryLayout<sockaddr_in>.size)

        let received = withUnsafeMutablePointer(to: &from) { addr in
            addr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                recvfrom(fd, &buffer, buffer.count, 0, sa, &fromLen)
            }
        }
        guard received > 0,
              let reply = String(bytes: buffer[0..<received], encoding: .utf8),
              reply.hasPrefix(OBSCProtocol.discoverReplyPrefix) else { return }

        // Reply format: OBSC_HERE:<port>:<name>
        let parts = reply.dropFirst(OBSCProtocol.discoverReplyPrefix.count)
            .split(separator: ":", maxSplits: 1, omittingEmptySubsequences: false)
        guard let port = UInt16(parts.first ?? "") else { return }
        let name = parts.count > 1 ? String(parts[1]) : "OBS"

        var hostBuffer = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
        var addr = from.sin_addr
        inet_ntop(AF_INET, &addr, &hostBuffer, socklen_t(INET_ADDRSTRLEN))
        let host = String(cString: hostBuffer)

        let server = Server(host: host, port: port, name: name)
        if !servers.contains(server) {
            servers.append(server)
            let snapshot = servers
            DispatchQueue.main.async { [weak self] in
                self?.onServersChanged?(snapshot)
            }
        }
    }
}
