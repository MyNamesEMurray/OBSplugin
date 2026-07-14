import SwiftUI
import AVFoundation

/// Live camera preview backed by AVCaptureVideoPreviewLayer, with optional
/// tap-to-focus (reports the tap in device coordinates) and pinch-to-zoom.
struct CameraPreviewView: UIViewRepresentable {
    let session: AVCaptureSession
    /// The camera's session queue. Attaching/detaching the preview layer
    /// takes the session's internal lock; doing it on the main thread while
    /// `stopRunning()` (1–3 s) holds that lock on the session queue freezes
    /// the whole UI — the classic "app hangs for a few seconds after
    /// stopping the stream". Routing both through the session queue
    /// serializes them after any start/stop instead of blocking main.
    let sessionQueue: DispatchQueue
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
        let layer = view.previewLayer
        layer.videoGravity = videoGravity
        // Off-main attach (see `sessionQueue` note). The preview stays
        // blank until the session is running anyway, so deferring the
        // attach behind a pending startRunning() costs nothing visible.
        let session = session
        sessionQueue.async {
            layer.session = session
        }

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

    static func dismantleUIView(_ uiView: PreviewView, coordinator: Coordinator) {
        // Off-main detach: when the streaming view closes right as
        // stopRunning() executes, detaching here on the main thread would
        // block until the stop completes (the 2–5 s hang). The queue hop
        // retains the layer until the detach has actually run.
        let layer = uiView.previewLayer
        coordinator.parent.sessionQueue.async {
            layer.session = nil
        }
    }
}
