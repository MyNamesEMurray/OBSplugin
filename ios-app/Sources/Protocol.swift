import Foundation

/// Wire protocol shared with the OBS plugin. See docs/PROTOCOL.md.
enum OBSCProtocol {
    static let magic: [UInt8] = [0x4F, 0x42, 0x53, 0x43] // "OBSC"
    static let version: UInt8 = 1
    static let headerSize = 20
    /// The app listens on this port; the OBS plugin dials it over the
    /// LAN or through usbmuxd (USB cable).
    static let usbPort: UInt16 = 9979

    /// Reference-audio format (fixed): 16 kHz mono signed-16-bit PCM.
    static let audioSampleRate = 16000
    static let audioChannels = 1

    enum PacketType: UInt8 {
        case hello = 1
        case videoConfig = 2
        case video = 3
        case ping = 4
        /// Latency clock sync: plugin sends REQ with its clock in pts;
        /// we echo that in the RESP payload with our clock in pts.
        case timesyncReq = 5
        case timesyncResp = 6
        /// Camera remote control, plugin → app (JSON payload).
        case control = 7
        /// Camera state report, app → plugin (JSON snapshot).
        case state = 8
        /// Reference audio, app → plugin: raw 16 kHz mono S16LE PCM,
        /// pts = capture time of the first sample (video clock domain).
        case audio = 9
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
