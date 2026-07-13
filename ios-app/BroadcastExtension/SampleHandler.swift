import ReplayKit
import CoreMedia
import AVFoundation

/// ReplayKit broadcast-upload extension: captures the whole screen (video +
/// system audio) and streams it to the OBS plugin over the same wire
/// protocol the camera app uses. The plugin dials port 9979 on the device;
/// usbmuxd tunnels that over USB exactly as it does for the camera, so
/// screen mirroring works over Wi-Fi *and* USB with no plugin transport
/// change.
///
/// System audio only (no microphone) by design: a streamer already has
/// their real mic in OBS, so sending the phone mic would double it. See
/// docs/PROTOCOL.md.
class SampleHandler: RPBroadcastSampleHandler {
    private let client = StreamClient()
    private var encoder: VideoEncoder?
    private var configuredWidth: Int32 = 0
    private var configuredHeight: Int32 = 0
    private let fps: Int32 = 60

    /// Canonical output: 48 kHz stereo signed-16-bit interleaved PCM.
    private let targetAudioFormat = AVAudioFormat(
        commonFormat: .pcmFormatInt16,
        sampleRate: Double(OBSCProtocol.screenAudioSampleRate),
        channels: AVAudioChannelCount(OBSCProtocol.screenAudioChannels),
        interleaved: true)!
    private var audioConverter: AVAudioConverter?
    private var audioSourceFormat: AVAudioFormat?

    // MARK: - Broadcast lifecycle

    override func broadcastStarted(withSetupInfo setupInfo: [String: NSObject]?) {
        client.sourceKind = .screen
        client.onStateChange = { [weak self] state in
            guard let self, state == .connected else { return }
            // Fresh connection: re-send config (dimensions are known once
            // the first frame arrives) and force a keyframe so OBS joins
            // immediately.
            if self.configuredWidth > 0 {
                self.client.sendVideoConfig(codec: .h264,
                                            width: self.configuredWidth,
                                            height: self.configuredHeight,
                                            fps: self.fps)
            }
            self.encoder?.requestKeyframe()
        }
        client.start(port: OBSCProtocol.usbPort)
    }

    override func broadcastFinished() {
        encoder?.stop()
        encoder = nil
        client.disconnect()
    }

    override func processSampleBuffer(_ sampleBuffer: CMSampleBuffer,
                                      with sampleBufferType: RPSampleBufferType) {
        switch sampleBufferType {
        case .video:
            handleVideo(sampleBuffer)
        case .audioApp:
            handleAudio(sampleBuffer)
        case .audioMic:
            break // omitted by design (see class note)
        @unknown default:
            break
        }
    }

    // MARK: - Video

    private func handleVideo(_ sampleBuffer: CMSampleBuffer) {
        guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        let width = Int32(CVPixelBufferGetWidth(pixelBuffer))
        let height = Int32(CVPixelBufferGetHeight(pixelBuffer))

        // (Re)build the encoder on first frame and whenever the screen
        // rotates (dimensions swap). The encoder has fixed dimensions.
        if encoder == nil || width != configuredWidth || height != configuredHeight {
            encoder?.stop()
            let enc = VideoEncoder(codec: .h264, width: width, height: height,
                                   fps: fps, bitrate: bitrate(width, height))
            enc.onEncodedFrame = { [weak self] frame in
                self?.client.sendVideoFrame(frame)
            }
            do {
                try enc.start()
            } catch {
                return
            }
            encoder = enc
            configuredWidth = width
            configuredHeight = height
            client.sendVideoConfig(codec: .h264, width: width, height: height,
                                   fps: fps)
        }
        encoder?.encode(sampleBuffer)
    }

    /// Screen content compresses well (lots of static regions), but detail
    /// scales with resolution; ~5 bits per pixel-second, clamped.
    private func bitrate(_ width: Int32, _ height: Int32) -> Int {
        let pixels = Int(width) * Int(height)
        return min(16_000_000, max(6_000_000, pixels * 5))
    }

    // MARK: - System audio

    private func handleAudio(_ sampleBuffer: CMSampleBuffer) {
        guard let formatDescription = CMSampleBufferGetFormatDescription(sampleBuffer),
              let sourceFormat = AVAudioFormat(cmAudioFormatDescription: formatDescription)
        else { return }

        if audioConverter == nil || audioSourceFormat != sourceFormat {
            audioConverter = AVAudioConverter(from: sourceFormat, to: targetAudioFormat)
            audioSourceFormat = sourceFormat
        }
        guard let converter = audioConverter else { return }

        let frames = CMSampleBufferGetNumSamples(sampleBuffer)
        guard frames > 0,
              let input = AVAudioPCMBuffer(pcmFormat: sourceFormat,
                                           frameCapacity: AVAudioFrameCount(frames))
        else { return }
        input.frameLength = input.frameCapacity
        guard CMSampleBufferCopyPCMDataIntoAudioBufferList(
            sampleBuffer, at: 0, frameCount: Int32(frames),
            into: input.mutableAudioBufferList) == noErr else { return }

        let ratio = targetAudioFormat.sampleRate / sourceFormat.sampleRate
        let capacity = AVAudioFrameCount(Double(frames) * ratio) + 32
        guard let output = AVAudioPCMBuffer(pcmFormat: targetAudioFormat,
                                            frameCapacity: capacity) else { return }

        var error: NSError?
        var provided = false
        converter.convert(to: output, error: &error) { _, status in
            if provided {
                status.pointee = .noDataNow
                return nil
            }
            provided = true
            status.pointee = .haveData
            return input
        }
        guard error == nil, output.frameLength > 0 else { return }

        // Interleaved target → a single buffer holds the PCM bytes.
        let buffer = output.audioBufferList.pointee.mBuffers
        guard let data = buffer.mData else { return }
        let pcm = Data(bytes: data, count: Int(buffer.mDataByteSize))

        // Same clock as the video frames' pts, so OBS keeps A/V in sync.
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let ptsNs = UInt64(max(0, CMTimeGetSeconds(pts)) * 1_000_000_000)
        client.sendScreenAudio(pcm, ptsNanoseconds: ptsNs)
    }
}
