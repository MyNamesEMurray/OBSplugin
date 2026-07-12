import Foundation
import VideoToolbox
import CoreMedia

enum VideoCodec: String, CaseIterable, Identifiable {
    case h264
    case hevc

    var id: String { rawValue }
    var label: String { self == .h264 ? "H.264" : "HEVC" }

    var vtCodecType: CMVideoCodecType {
        self == .h264 ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC
    }
}

/// Hardware video encoder (VideoToolbox). Emits Annex B access units;
/// keyframes are self-contained (parameter sets prepended: SPS/PPS for
/// H.264, VPS/SPS/PPS for HEVC) so the OBS plugin can join at any keyframe.
final class VideoEncoder {
    struct EncodedFrame {
        let data: Data
        let ptsNanoseconds: UInt64
        let isKeyframe: Bool
    }

    var onEncodedFrame: ((EncodedFrame) -> Void)?

    let codec: VideoCodec

    private var session: VTCompressionSession?
    private let width: Int32
    private let height: Int32
    private let fps: Int32
    private let bitrate: Int

    private static let startCode = Data([0x00, 0x00, 0x00, 0x01])

    init(codec: VideoCodec, width: Int32, height: Int32, fps: Int32, bitrate: Int) {
        self.codec = codec
        self.width = width
        self.height = height
        self.fps = fps
        self.bitrate = bitrate
    }

    /// Whether this device can hardware-encode the codec (HEVC needs A10+).
    static func isSupported(_ codec: VideoCodec) -> Bool {
        if codec == .h264 { return true }
        var session: VTCompressionSession?
        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            width: 1280, height: 720,
            codecType: codec.vtCodecType,
            encoderSpecification: nil,
            imageBufferAttributes: nil,
            compressedDataAllocator: nil,
            outputCallback: nil, refcon: nil,
            compressionSessionOut: &session)
        if let session {
            VTCompressionSessionInvalidate(session)
        }
        return status == noErr
    }

    func start() throws {
        var session: VTCompressionSession?
        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            width: width,
            height: height,
            codecType: codec.vtCodecType,
            encoderSpecification: nil,
            imageBufferAttributes: nil,
            compressedDataAllocator: nil,
            outputCallback: nil,
            refcon: nil,
            compressionSessionOut: &session)

        guard status == noErr, let session else {
            throw NSError(domain: "VideoEncoder", code: Int(status),
                          userInfo: [NSLocalizedDescriptionKey: "\(codec.label) encoder unavailable (\(status))"])
        }

        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_RealTime,
                             value: kCFBooleanTrue)
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_ProfileLevel,
                             value: codec == .h264 ? kVTProfileLevel_H264_Main_AutoLevel
                                                   : kVTProfileLevel_HEVC_Main_AutoLevel)
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AllowFrameReordering,
                             value: kCFBooleanFalse)
        // Shave per-frame encode time; quality difference is negligible
        // at streaming bitrates.
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality,
                             value: kCFBooleanTrue)
        // Don't let the encoder sit on frames internally.
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_MaxFrameDelayCount,
                             value: NSNumber(value: 1))
        Self.applyBitrate(bitrate, to: session)
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_ExpectedFrameRate,
                             value: NSNumber(value: fps))
        // Keyframe at least every 2 seconds so joins/recoveries are quick.
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_MaxKeyFrameInterval,
                             value: NSNumber(value: fps * 2))
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration,
                             value: NSNumber(value: 2))

        VTCompressionSessionPrepareToEncodeFrames(session)
        self.session = session
    }

    /// Adjusts the target bitrate mid-stream (adaptive bitrate).
    func setBitrate(_ bitsPerSecond: Int) {
        guard let session else { return }
        Self.applyBitrate(bitsPerSecond, to: session)
    }

    /// Average target plus a HARD cap (1.5x average over 1-second windows).
    /// Without the cap, hard-to-compress content — e.g. sensor noise
    /// magnified by digital zoom — overshoots the average enough to
    /// saturate a Wi-Fi link.
    private static func applyBitrate(_ bitsPerSecond: Int,
                                     to session: VTCompressionSession) {
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AverageBitRate,
                             value: NSNumber(value: bitsPerSecond))
        let bytesPerSecondCap = bitsPerSecond * 3 / 16 // (bps / 8) * 1.5
        let limits: [NSNumber] = [NSNumber(value: bytesPerSecondCap),
                                  NSNumber(value: 1.0)]
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_DataRateLimits,
                             value: limits as CFArray)
    }

    func stop() {
        guard let session else { return }
        VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
        VTCompressionSessionInvalidate(session)
        self.session = nil
    }

    /// Request that the next encoded frame is a keyframe (e.g. when a new
    /// connection is established mid-stream).
    private var forceNextKeyframe = false
    func requestKeyframe() {
        forceNextKeyframe = true
    }

    func encode(_ sampleBuffer: CMSampleBuffer) {
        guard let session,
              let imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }

        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let duration = CMSampleBufferGetDuration(sampleBuffer)

        var frameProperties: CFDictionary?
        if forceNextKeyframe {
            forceNextKeyframe = false
            frameProperties = [kVTEncodeFrameOptionKey_ForceKeyFrame: kCFBooleanTrue!] as CFDictionary
        }

        VTCompressionSessionEncodeFrame(
            session,
            imageBuffer: imageBuffer,
            presentationTimeStamp: pts,
            duration: duration,
            frameProperties: frameProperties,
            infoFlagsOut: nil
        ) { [weak self] status, _, sampleBuffer in
            guard status == noErr, let sampleBuffer else { return }
            self?.emit(sampleBuffer)
        }
    }

    private func emit(_ sampleBuffer: CMSampleBuffer) {
        guard CMSampleBufferDataIsReady(sampleBuffer) else { return }

        let isKeyframe = !sampleBufferIsNotSync(sampleBuffer)
        guard let annexB = annexBData(from: sampleBuffer, includeParameterSets: isKeyframe) else { return }

        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let ptsNs = UInt64(max(0, CMTimeGetSeconds(pts)) * 1_000_000_000)

        onEncodedFrame?(EncodedFrame(data: annexB, ptsNanoseconds: ptsNs, isKeyframe: isKeyframe))
    }

    private func sampleBufferIsNotSync(_ sampleBuffer: CMSampleBuffer) -> Bool {
        guard let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false) as? [[CFString: Any]],
              let first = attachments.first else {
            return false
        }
        return (first[kCMSampleAttachmentKey_NotSync] as? Bool) ?? false
    }

    private func parameterSetCount(_ formatDescription: CMFormatDescription) -> Int {
        var count = 0
        switch codec {
        case .h264:
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                formatDescription, parameterSetIndex: 0,
                parameterSetPointerOut: nil, parameterSetSizeOut: nil,
                parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
        case .hevc:
            CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                formatDescription, parameterSetIndex: 0,
                parameterSetPointerOut: nil, parameterSetSizeOut: nil,
                parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
        }
        return count
    }

    private func parameterSet(_ formatDescription: CMFormatDescription,
                              index: Int) -> (UnsafePointer<UInt8>, Int)? {
        var pointer: UnsafePointer<UInt8>?
        var size = 0
        let status: OSStatus
        switch codec {
        case .h264:
            status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                formatDescription, parameterSetIndex: index,
                parameterSetPointerOut: &pointer, parameterSetSizeOut: &size,
                parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil)
        case .hevc:
            status = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                formatDescription, parameterSetIndex: index,
                parameterSetPointerOut: &pointer, parameterSetSizeOut: &size,
                parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil)
        }
        guard status == noErr, let pointer else { return nil }
        return (pointer, size)
    }

    /// Converts an AVCC (length-prefixed) sample buffer into Annex B,
    /// optionally prepending the codec's parameter sets.
    private func annexBData(from sampleBuffer: CMSampleBuffer,
                            includeParameterSets: Bool) -> Data? {
        var result = Data()

        if includeParameterSets,
           let formatDescription = CMSampleBufferGetFormatDescription(sampleBuffer) {
            for index in 0..<parameterSetCount(formatDescription) {
                guard let (pointer, size) = parameterSet(formatDescription, index: index) else {
                    return nil
                }
                result.append(Self.startCode)
                result.append(pointer, count: size)
            }
        }

        guard let blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else { return nil }

        var totalLength = 0
        var dataPointer: UnsafeMutablePointer<CChar>?
        let status = CMBlockBufferGetDataPointer(
            blockBuffer, atOffset: 0, lengthAtOffsetOut: nil,
            totalLengthOut: &totalLength, dataPointerOut: &dataPointer)
        guard status == kCMBlockBufferNoErr, let dataPointer else { return nil }

        // Walk the AVCC buffer: [4-byte BE length][NAL] ... → start codes.
        var offset = 0
        let lengthPrefixSize = 4
        dataPointer.withMemoryRebound(to: UInt8.self, capacity: totalLength) { bytes in
            while offset + lengthPrefixSize <= totalLength {
                let nalLength = Int(bytes[offset]) << 24
                    | Int(bytes[offset + 1]) << 16
                    | Int(bytes[offset + 2]) << 8
                    | Int(bytes[offset + 3])
                offset += lengthPrefixSize
                guard nalLength > 0, offset + nalLength <= totalLength else { break }
                result.append(Self.startCode)
                result.append(bytes + offset, count: nalLength)
                offset += nalLength
            }
        }

        return result.isEmpty ? nil : result
    }
}
