import SwiftUI
import AVFoundation

/// Live camera preview backed by AVCaptureVideoPreviewLayer, with optional
/// tap-to-focus (reports the tap in device coordinates) and pinch-to-zoom.
struct CameraPreviewView: UIViewRepresentable {
    let session: AVCaptureSession
    var videoGravity: AVLayerVideoGravity = .resizeAspect
    var onTapAtDevicePoint: ((CGPoint) -> Void)?
    var onPinchZoom: ((_ phase: PinchPhase, _ scale: CGFloat) -> Void)?

    enum PinchPhase {
        case began
        case changed
        case ended
    }

    final class PreviewView: UIView {
        override static var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
        var previewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
    }

    final class Coordinator: NSObject {
        var parent: CameraPreviewView

        init(parent: CameraPreviewView) {
            self.parent = parent
        }

        @objc func handleTap(_ recognizer: UITapGestureRecognizer) {
            guard let view = recognizer.view as? PreviewView else { return }
            let layerPoint = recognizer.location(in: view)
            let devicePoint = view.previewLayer
                .captureDevicePointConverted(fromLayerPoint: layerPoint)
            parent.onTapAtDevicePoint?(devicePoint)
        }

        @objc func handlePinch(_ recognizer: UIPinchGestureRecognizer) {
            let phase: PinchPhase
            switch recognizer.state {
            case .began:
                phase = .began
            case .ended, .cancelled, .failed:
                phase = .ended
            default:
                phase = .changed
            }
            parent.onPinchZoom?(phase, recognizer.scale)
        }
    }

    func makeCoordinator() -> Coordinator {
        Coordinator(parent: self)
    }

    func makeUIView(context: Context) -> PreviewView {
        let view = PreviewView()
        view.previewLayer.session = session
        view.previewLayer.videoGravity = videoGravity

        if onTapAtDevicePoint != nil {
            let tap = UITapGestureRecognizer(
                target: context.coordinator,
                action: #selector(Coordinator.handleTap(_:)))
            view.addGestureRecognizer(tap)
        }
        if onPinchZoom != nil {
            let pinch = UIPinchGestureRecognizer(
                target: context.coordinator,
                action: #selector(Coordinator.handlePinch(_:)))
            view.addGestureRecognizer(pinch)
        }
        return view
    }

    func updateUIView(_ uiView: PreviewView, context: Context) {
        context.coordinator.parent = self
        uiView.previewLayer.videoGravity = videoGravity
    }
}
