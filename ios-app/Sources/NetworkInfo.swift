import Foundation
import Darwin

enum NetworkInfo {
    /// The device's Wi-Fi (en0) IPv4 address, e.g. "192.168.1.42".
    static func wifiIPAddress() -> String? {
        var result: String?
        var ifaddrPointer: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddrPointer) == 0, let first = ifaddrPointer else {
            return nil
        }
        defer { freeifaddrs(ifaddrPointer) }

        for pointer in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let interface = pointer.pointee
            guard let addr = interface.ifa_addr,
                  addr.pointee.sa_family == sa_family_t(AF_INET),
                  String(cString: interface.ifa_name) == "en0" else {
                continue
            }

            var buffer = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
            addr.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { sin in
                var sinAddr = sin.pointee.sin_addr
                inet_ntop(AF_INET, &sinAddr, &buffer, socklen_t(INET_ADDRSTRLEN))
            }
            result = String(cString: buffer)
            break
        }
        return result
    }
}
