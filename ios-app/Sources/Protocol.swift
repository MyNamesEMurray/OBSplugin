import Foundation

/// Wire protocol shared with the OBS plugin. See docs/PROTOCOL.md.
enum OBSCProtocol {
    static let magic: [UInt8] = [0x4F, 0x42, 0x53, 0x43] // "OBSC"
    static let version: UInt8 = 1
    static let headerSize = 20
    static let defaultPort: UInt16 = 9977
    static let discoveryPortOffset: UInt16 = 1
    static let discoverRequest = "OBSC_DISCOVER"
    static let discoverReplyPrefix = "OBSC_HERE:"

    enum PacketType: UInt8 {
        case hello = 1
        case videoConfig = 2
        case video = 3
        case ping = 4
    }

    struct Flags: OptionSet {
        let rawValue: UInt16
        static let keyframe = Flags(rawValue: 0x0001)
    }

    /// Builds a framed packet: 20-byte big-endian header + payload.
    static func packet(type: PacketType,
                       flags: Flags = [],
                       ptsNanoseconds: UInt64 = 0,
                       payload: Data) -> Data {
        var data = Data(capacity: headerSize + payload.count)
        data.append(contentsOf: magic)
        data.append(version)
        data.append(type.rawValue)
        appendBigEndian(&data, flags.rawValue)
        appendBigEndian(&data, ptsNanoseconds)
        appendBigEndian(&data, UInt32(payload.count))
        data.append(payload)
        return data
    }

    private static func appendBigEndian<T: FixedWidthInteger>(_ data: inout Data, _ value: T) {
        withUnsafeBytes(of: value.bigEndian) { data.append(contentsOf: $0) }
    }
}
