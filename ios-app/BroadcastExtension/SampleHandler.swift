import ReplayKit
import CoreMedia
import AVFoundation
import os

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

    /// Screen mirroring uses H.264 by default. HEVC compresses screen/UI
    /// content ~40% smaller at the same quality, so the wire bitrate drops
    /// — flip this to `true` to A/B whether HEVC improves reliability. The
    /// device must support HEVC hardware encoding (A10 / iPhone 7+); we
    /// fall back to H.264 otherwise, and the plugin decodes whichever codec
    /// the video config announces.
    private static let preferHEVC = false
    // static let → initialized exactly once, thread-safely (read from the
    // capture thread and the network queue).
    private static let codec: VideoCodec =
        (preferHEVC && VideoEncoder.isSupported(.hevc)) ? .hevc : .h264

    private let log = Logger(subsystem: "com.exaltedpixels.LensLinkCamera.broadcast",
                             category: "diag")

    /// Pipeline counters, surfaced every few seconds to the OBS log (via the
    /// plugin) and os_log. Guarded by `countersLock`: `processSampleBuffer`
    /// and the encoder callback touch them from different threads while the
    /// heartbeat task reads them.
    private struct Counters {
        var videoSamples = 0
        var encoderBuilds = 0
        var encodedFrames = 0
        var encodedKeyframes = 0
        var encoderErrors = 0
        var audioSamples = 0
        var dims = ""
        var codecName = ""
        /// Set when the plugin asks for a keyframe (e.g. after it swaps to
        /// software decoding); consumed on the capture thread, which owns
        /// the encoder. Guarded by the same lock so we never touch the
        /// encoder from the network queue.
        var pendingKeyframe = false
    }
    private let countersLock = NSLock()
    private var counters = Counters()
    private func withCounters<T>(_ body: (inout Counters) -> T) -> T {
        countersLock.lock()
        defer { countersLock.unlock() }
        return body(&counters)
    }
    private var diagTask: Task<Void, Never>?

    /// Canonical output: 48 kHz stereo signed-16-bit interleaved PCM.
    private let targetAudioFormat = AVAudioFormat(
        commonFormat: .pcmFormatInt16,
        sampleRate: Double(OBSCProtocol.screenAudioSampleRate),
        channels: AVAudioChannelCount(OBSCProtocol.screenAudioChannels),
        interleaved: true)!
    private var audioConverter: AVAudioConverter?
    private var audioSourceFormat: AVAudioFormat?

    // MARK: - Broadcast lifecycle

    /// Extensions have no UI, so failures here are invisible unless we end
    /// the broadcast with a descriptive error (iOS shows it as an alert).
    private var everConnected = false
    private var watchdog: Task<Void, Never>?

    private func fail(_ message: String) {
        finishBroadcastWithError(NSError(
            domain: "LensLink", code: 1,
            userInfo: [NSLocalizedDescriptionKey: message]))
    }

    override func broadcastStarted(withSetupInfo setupInfo: [String: NSObject]?) {
        client.sourceKind = .screen
        client.onControl = { [weak self] data in
            self?.handleControl(data)
        }
        client.onStateChange = { [weak self] state in
            guard let self else { return }
            switch state {
            case .connected:
                self.everConnected = true
                self.watchdog?.cancel()
                // Fresh connection: re-send config (dimensions are known
                // once the first frame arrives) and force a keyframe so
                // OBS joins immediately.
                if self.configuredWidth > 0 {
                    self.client.sendVideoConfig(codec: Self.codec,
                                                width: self.configuredWidth,
                                                height: self.configuredHeight,
                                                fps: self.fps)
                }
                self.encoder?.requestKeyframe()
            case .failed(let message):
                // Most likely: port 9979 already taken because the LensLink
                // app's camera stream is running. One stream per device.
                self.fail("Could not open the streaming port (\(message)). "
                    + "If the LensLink camera is streaming, stop it first — "
                    + "a device can send the camera or the screen, not both.")
            default:
                break
            }
        }
        client.start(port: OBSCProtocol.usbPort)
        log.info("broadcast started, codec=\(Self.codec.rawValue, privacy: .public)")
        startDiagnostics()

        // OBS never dialing in used to fail silently; surface it instead,
        // with the listener's actual state so the alert pinpoints which
        // layer broke (listener never ready / no connections arrived /
        // handshake failed).
        watchdog = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 30_000_000_000)
            guard let self, !Task.isCancelled, !self.everConnected else { return }
            self.fail("OBS did not connect within 30 seconds "
                + "[\(self.client.debugStatus())]. Check the LensLink "
                + "Camera source points at this phone (USB, or this "
                + "phone's Wi-Fi IP) and the camera stream isn't running.")
        }
    }

    override func broadcastFinished() {
        watchdog?.cancel()
        diagTask?.cancel()
        encoder?.stop()
        encoder = nil
        client.disconnect()
    }

    /// Emits a pipeline heartbeat every 3 s: how many screen samples came in,
    /// how many frames we encoded and sent, plus the network snapshot. Goes
    /// to the OBS log (through the plugin) and os_log, so a stuck stream
    /// shows exactly which stage stopped moving. The plugin logs its own
    /// matching line, so both ends line up in one place.
    private func startDiagnostics() {
        diagTask?.cancel()
        diagTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 3_000_000_000)
                guard let self, !Task.isCancelled else { return }
                let c = self.withCounters { $0 }
                let line = "vid samp=\(c.videoSamples) enc=\(c.encodedFrames) "
                    + "kf=\(c.encodedKeyframes) builds=\(c.encoderBuilds) "
                    + "encErr=\(c.encoderErrors) "
                    + "\(c.codecName)\(c.dims.isEmpty ? "" : " " + c.dims) "
                    + "aud=\(c.audioSamples) | " + self.client.diagnosticsSnapshot()
                self.client.sendDiag(line)
                self.log.info("\(line, privacy: .public)")
            }
        }
    }

    /// Control messages from the plugin. The only one the screen extension
    /// acts on is a keyframe request (the plugin sends it after switching to
    /// software decoding so the picture comes back right away).
    private func handleControl(_ data: Data) {
        guard let obj = try? JSONSerialization.jsonObject(with: data)
                as? [String: Any],
              let cmd = obj["cmd"] as? String else { return }
        if cmd == "keyframe" {
            withCounters { $0.pendingKeyframe = true }
            log.info("keyframe requested by plugin")
        }
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
        withCounters { $0.videoSamples += 1 }

        // (Re)build the encoder on first frame and whenever the screen
        // rotates (dimensions swap). The encoder has fixed dimensions.
        if encoder == nil || width != configuredWidth || height != configuredHeight {
            encoder?.stop()
            let enc = VideoEncoder(codec: Self.codec, width: width, height: height,
                                   fps: fps, bitrate: bitrate(width, height))
            enc.onEncodedFrame = { [weak self] frame in
                guard let self else { return }
                self.withCounters {
                    $0.encodedFrames += 1
                    if frame.isKeyframe { $0.encodedKeyframes += 1 }
                }
                self.client.sendVideoFrame(frame)
            }
            do {
                try enc.start()
            } catch {
                withCounters { $0.encoderErrors += 1 }
                log.error("encoder start failed at \(width)x\(height): \(error.localizedDescription, privacy: .public)")
                return
            }
            encoder = enc
            configuredWidth = width
            configuredHeight = height
            let br = bitrate(width, height)
            withCounters {
                $0.encoderBuilds += 1
                $0.dims = "\(width)x\(height)"
                $0.codecName = Self.codec.rawValue
            }
            log.info("encoder built \(width)x\(height) \(Self.codec.rawValue, privacy: .public) @ \(br / 1_000_000) Mbps")
            client.sendVideoConfig(codec: Self.codec, width: width, height: height,
                                   fps: fps)
        }
        // Honour a pending keyframe request here, on the capture thread that
        // owns the encoder (never from the network queue).
        let forceKeyframe = withCounters { (c: inout Counters) -> Bool in
            defer { c.pendingKeyframe = false }
            return c.pendingKeyframe
        }
        if forceKeyframe {
            encoder?.requestKeyframe()
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
        withCounters { $0.audioSamples += 1 }
        guard let formatDescription = CMSampleBufferGetFormatDescription(sampleBuffer)
        else { return }
        // Non-failable initializer — returns AVAudioFormat, not an optional.
        let sourceFormat = AVAudioFormat(cmAudioFormatDescription: formatDescription)

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
