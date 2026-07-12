import Foundation
import AVFoundation
import UIKit

/// Owns the AVCaptureSession and delivers raw camera frames.
final class CameraManager: NSObject {
    enum Resolution: String, CaseIterable, Identifiable {
        case hd720 = "720p"
        case hd1080 = "1080p"

        var id: String { rawValue }

        var preset: AVCaptureSession.Preset {
            switch self {
            case .hd720: return .hd1280x720
            case .hd1080: return .hd1920x1080
            }
        }

        var size: (width: Int32, height: Int32) {
            switch self {
            case .hd720: return (1280, 720)
            case .hd1080: return (1920, 1080)
            }
        }

        var defaultBitrate: Int {
            switch self {
            case .hd720: return 4_000_000
            case .hd1080: return 8_000_000
            }
        }
    }

    let session = AVCaptureSession()
    var onSampleBuffer: ((CMSampleBuffer) -> Void)?

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

    func configure(position: AVCaptureDevice.Position,
                   resolution: Resolution,
                   fps: Int32) throws {
        session.beginConfiguration()
        defer { session.commitConfiguration() }

        session.inputs.forEach(session.removeInput)
        session.outputs.forEach(session.removeOutput)

        session.sessionPreset = resolution.preset

        guard let device = AVCaptureDevice.default(.builtInWideAngleCamera,
                                                   for: .video,
                                                   position: position) else {
            throw NSError(domain: "CameraManager", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "Camera not available"])
        }

        let input = try AVCaptureDeviceInput(device: device)
        guard session.canAddInput(input) else {
            throw NSError(domain: "CameraManager", code: 2,
                          userInfo: [NSLocalizedDescriptionKey: "Cannot add camera input"])
        }
        session.addInput(input)

        try device.lockForConfiguration()
        let frameDuration = CMTime(value: 1, timescale: fps)
        if device.activeFormat.videoSupportedFrameRateRanges
            .contains(where: { $0.maxFrameRate >= Double(fps) }) {
            device.activeVideoMinFrameDuration = frameDuration
            device.activeVideoMaxFrameDuration = frameDuration
        }
        device.unlockForConfiguration()

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
