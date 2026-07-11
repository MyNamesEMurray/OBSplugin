import Foundation
import VideoToolbox
import CoreMedia

/// Hardware H.264 encoder (VideoToolbox). Emits Annex B access units;
/// keyframes are self-contained (SPS/PPS prepended) so the OBS plugin can
/// join the stream at any keyframe.
final class H264Encoder {
    struct EncodedFrame {
        let data: Data
        let ptsNanoseconds: UInt64
        let isKeyframe: Bool
    }

    var onEncodedFrame: ((EncodedFrame) -> Void)?

    private var session: VTCompressionSession?
    private let width: Int32
    private let height: Int32
    private let fps: Int32
    private let bitrate: Int

    private static let startCode = Data([0x00, 0x00, 0x00, 0x01])

    init(width: Int32, height: Int32, fps: Int32, bitrate: Int) {
        self.width = width
        self.height = height
        self.fps = fps
        self.bitrate = bitrate
    }

    func start() throws {
        var session: VTCompressionSession?
        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            width: width,
            height: height,
            codecType: kCMVideoCodecType_H264,
            encoderSpecification: nil,
            imageBufferAttributes: nil,
            compressedDataAllocator: nil,
            outputCallback: nil,
            refcon: nil,
            compressionSessionOut: &session)

        guard status == noErr, let session else {
            throw NSError(domain: "H264Encoder", code: Int(status),
                          userInfo: [NSLocalizedDescriptionKey: "VTCompressionSessionCreate failed (\(status))"])
        }

        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_RealTime,
                             value: kCFBooleanTrue)
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_ProfileLevel,
                             value: kVTProfileLevel_H264_Main_AutoLevel)
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AllowFrameReordering,
                             value: kCFBooleanFalse)
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AverageBitRate,
                             value: NSNumber(value: bitrate))
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
        guard let annexB = Self.annexBData(from: sampleBuffer, includeParameterSets: isKeyframe) else { return }

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

    /// Converts an AVCC (length-prefixed) sample buffer into Annex B,
    /// optionally prepending SPS/PPS from the format description.
    private static func annexBData(from sampleBuffer: CMSampleBuffer,
                                   includeParameterSets: Bool) -> Data? {
        var result = Data()

        if includeParameterSets,
           let formatDescription = CMSampleBufferGetFormatDescription(sampleBuffer) {
            var parameterSetCount = 0
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                formatDescription, parameterSetIndex: 0,
                parameterSetPointerOut: nil, parameterSetSizeOut: nil,
                parameterSetCountOut: &parameterSetCount, nalUnitHeaderLengthOut: nil)

            for index in 0..<parameterSetCount {
                var pointer: UnsafePointer<UInt8>?
                var size = 0
                let status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    formatDescription, parameterSetIndex: index,
                    parameterSetPointerOut: &pointer, parameterSetSizeOut: &size,
                    parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil)
                guard status == noErr, let pointer else { return nil }
                result.append(startCode)
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
                result.append(startCode)
                result.append(bytes + offset, count: nalLength)
                offset += nalLength
            }
        }

        return result.isEmpty ? nil : result
    }
}
