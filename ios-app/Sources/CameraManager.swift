import Foundation
import AVFoundation
import UIKit

/// Owns the AVCaptureSession and delivers raw camera frames.
final class CameraManager: NSObject {
    enum Resolution: String, CaseIterable, Identifiable {
        case hd720 = "720p"
        case hd1080 = "1080p"
        case uhd4k = "4K"

        var id: String { rawValue }

        var size: (width: Int32, height: Int32) {
            switch self {
            case .hd720: return (1280, 720)
            case .hd1080: return (1920, 1080)
            case .uhd4k: return (3840, 2160)
            }
        }

        func bitrate(for codec: VideoCodec) -> Int {
            let h264: Int
            switch self {
            case .hd720: h264 = 4_000_000
            case .hd1080: h264 = 8_000_000
            case .uhd4k: h264 = 30_000_000
            }
            // HEVC reaches comparable quality at roughly 60% of the bits.
            return codec == .hevc ? h264 * 6 / 10 : h264
        }
    }

    let session = AVCaptureSession()
    var onSampleBuffer: ((CMSampleBuffer) -> Void)?

    /// The device currently feeding the session; camera controls act on it.
    private(set) var activeDevice: AVCaptureDevice?

    private let sessionQueue = DispatchQueue(label: "obscam.session")
    private let videoQueue = DispatchQueue(label: "obscam.video")
    private var videoOutput: AVCaptureVideoDataOutput?

    static func requestPermission() async -> Bool {
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            return true
        case .notDetermined:
            return await AVCaptureDevice.requestAccess(for: .video)
        default:
            return false
        }
    }

    static func device(for position: AVCaptureDevice.Position) -> AVCaptureDevice? {
        AVCaptureDevice.default(.builtInWideAngleCamera, for: .video,
                                position: position)
    }

    private static func format(for device: AVCaptureDevice,
                               resolution: Resolution,
                               fps: Int32) -> AVCaptureDevice.Format? {
        let target = resolution.size
        // Prefer earlier (unbinned, video-range) formats; require exact
        // dimensions and a frame-rate range covering the requested rate.
        return device.formats.first { format in
            let dims = CMVideoFormatDescriptionGetDimensions(format.formatDescription)
            guard dims.width == target.width, dims.height == target.height else {
                return false
            }
            return format.videoSupportedFrameRateRanges
                .contains { $0.maxFrameRate >= Double(fps) }
        }
    }

    /// Whether this camera can capture the resolution at the frame rate.
    /// Drives the app's pickers so unsupported combos are never offered.
    static func supports(resolution: Resolution, fps: Int32,
                         position: AVCaptureDevice.Position) -> Bool {
        guard let device = device(for: position) else { return false }
        return format(for: device, resolution: resolution, fps: fps) != nil
    }

    func configure(position: AVCaptureDevice.Position,
                   resolution: Resolution,
                   fps: Int32) throws {
        session.beginConfiguration()
        defer { session.commitConfiguration() }

        session.inputs.forEach(session.removeInput)
        session.outputs.forEach(session.removeOutput)

        // Format is chosen manually below; presets can't express 4K60.
        session.sessionPreset = .inputPriority

        guard let device = Self.device(for: position) else {
            throw NSError(domain: "CameraManager", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "Camera not available"])
        }
        guard let format = Self.format(for: device, resolution: resolution, fps: fps) else {
            throw NSError(domain: "CameraManager", code: 4,
                          userInfo: [NSLocalizedDescriptionKey:
                            "\(resolution.rawValue) at \(fps) fps is not supported by this camera"])
        }

        let input = try AVCaptureDeviceInput(device: device)
        guard session.canAddInput(input) else {
            throw NSError(domain: "CameraManager", code: 2,
                          userInfo: [NSLocalizedDescriptionKey: "Cannot add camera input"])
        }
        session.addInput(input)

        try device.lockForConfiguration()
        device.activeFormat = format
        let frameDuration = CMTime(value: 1, timescale: fps)
        device.activeVideoMinFrameDuration = frameDuration
        device.activeVideoMaxFrameDuration = frameDuration
        device.unlockForConfiguration()
        activeDevice = device

        let output = AVCaptureVideoDataOutput()
        output.videoSettings = [
            kCVPixelBufferPixelFormatTypeKey as String:
                kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        ]
        output.alwaysDiscardsLateVideoFrames = true
        output.setSampleBufferDelegate(self, queue: videoQueue)

        guard session.canAddOutput(output) else {
            throw NSError(domain: "CameraManager", code: 3,
                          userInfo: [NSLocalizedDescriptionKey: "Cannot add video output"])
        }
        session.addOutput(output)
        videoOutput = output

        if let connection = output.connection(with: .video) {
            if connection.isVideoOrientationSupported {
                connection.videoOrientation = .landscapeRight
            }
            // Stabilization buffers multiple frames inside the capture
            // pipeline — a large hidden latency cost for a live feed.
            if connection.isVideoStabilizationSupported {
                connection.preferredVideoStabilizationMode = .off
            }
            if position == .front, connection.isVideoMirroringSupported {
                connection.isVideoMirrored = true
            }
        }
    }

    // MARK: - Live camera controls

    /// Runs `block` with the active device locked for configuration.
    private func withLockedDevice(_ block: (AVCaptureDevice) -> Void) {
        guard let device = activeDevice,
              (try? device.lockForConfiguration()) != nil else { return }
        block(device)
        device.unlockForConfiguration()
    }

    var maxZoomFactor: CGFloat {
        guard let device = activeDevice else { return 1 }
        // Beyond ~10x the digital zoom is mush; keep the slider useful.
        return min(device.activeFormat.videoMaxZoomFactor, 10)
    }

    func setZoom(_ factor: CGFloat) {
        withLockedDevice { device in
            let clamped = max(device.minAvailableVideoZoomFactor,
                              min(factor, maxZoomFactor))
            device.videoZoomFactor = clamped
        }
    }

    var exposureBiasRange: ClosedRange<Float> {
        guard let device = activeDevice else { return -2...2 }
        let lower = max(device.minExposureTargetBias, -3)
        let upper = min(device.maxExposureTargetBias, 3)
        return lower...max(upper, lower + 0.1)
    }

    func setExposureBias(_ bias: Float) {
        withLockedDevice { device in
            let clamped = max(device.minExposureTargetBias,
                              min(bias, device.maxExposureTargetBias))
            device.setExposureTargetBias(clamped)
        }
    }

    func setContinuousAutoFocus() {
        withLockedDevice { device in
            if device.isFocusModeSupported(.continuousAutoFocus) {
                device.focusMode = .continuousAutoFocus
            }
            if device.isExposureModeSupported(.continuousAutoExposure) {
                device.exposureMode = .continuousAutoExposure
            }
        }
    }

    /// Locks focus, optionally at a specific lens position (0 = closest,
    /// 1 = infinity). Without a position, freezes focus where it is.
    func lockFocus(lensPosition: Float?) {
        withLockedDevice { device in
            if let lensPosition,
               device.isLockingFocusWithCustomLensPositionSupported {
                device.setFocusModeLocked(
                    lensPosition: max(0, min(lensPosition, 1)))
            } else if device.isFocusModeSupported(.locked) {
                device.focusMode = .locked
            }
        }
    }

    /// One-shot focus + exposure at a point of interest (0…1 device coords).
    func focusAndExpose(at devicePoint: CGPoint) {
        withLockedDevice { device in
            if device.isFocusPointOfInterestSupported,
               device.isFocusModeSupported(.continuousAutoFocus) {
                device.focusPointOfInterest = devicePoint
                device.focusMode = .continuousAutoFocus
            }
            if device.isExposurePointOfInterestSupported,
               device.isExposureModeSupported(.continuousAutoExposure) {
                device.exposurePointOfInterest = devicePoint
                device.exposureMode = .continuousAutoExposure
            }
        }
    }

    var hasTorch: Bool { activeDevice?.hasTorch ?? false }

    func setTorch(_ on: Bool) {
        withLockedDevice { device in
            guard device.hasTorch else { return }
            device.torchMode = on ? .on : .off
        }
    }

    func start() {
        sessionQueue.async { [session] in
            if !session.isRunning {
                session.startRunning()
            }
        }
    }

    func stop() {
        sessionQueue.async { [session] in
            if session.isRunning {
                session.stopRunning()
            }
        }
    }
}

extension CameraManager: AVCaptureVideoDataOutputSampleBufferDelegate {
    func captureOutput(_ output: AVCaptureOutput,
                       didOutput sampleBuffer: CMSampleBuffer,
                       from connection: AVCaptureConnection) {
        onSampleBuffer?(sampleBuffer)
    }
}
