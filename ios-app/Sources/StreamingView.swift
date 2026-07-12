import SwiftUI
import AVFoundation

/// Full-screen live view shown while streaming: camera preview with
/// tap-to-focus and pinch-to-zoom, camera controls, and optional
/// battery-saving dimming.
struct StreamingView: View {
    @EnvironmentObject private var streamer: Streamer

    @State private var dimmed = false
    @State private var lastInteraction = Date()
    @State private var pinchBaseZoom: CGFloat = 1
    @State private var previousBrightness: CGFloat = UIScreen.main.brightness

    private static let dimAfterSeconds: TimeInterval = 10

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            CameraPreviewView(
                session: streamer.camera.session,
                videoGravity: .resizeAspect,
                onTapAtDevicePoint: { point in
                    touched()
                    streamer.camera.focusAndExpose(at: point)
                },
                onPinchZoom: { scale, ended in
                    touched()
                    streamer.zoom = pinchBaseZoom * scale
                    if ended {
                        pinchBaseZoom = streamer.zoom
                    }
                }
            )
            .ignoresSafeArea()

            VStack {
                statusBar
                Spacer()
                controlPanel
            }
            .padding()

            if dimmed {
                dimOverlay
            }
        }
        .statusBar(hidden: true)
        .task {
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                if streamer.dimWhileStreaming && !dimmed &&
                    Date().timeIntervalSince(lastInteraction) > Self.dimAfterSeconds {
                    dim()
                }
            }
        }
        .onDisappear {
            if dimmed {
                UIScreen.main.brightness = previousBrightness
            }
        }
    }

    private func touched() {
        lastInteraction = Date()
    }

    private func dim() {
        previousBrightness = UIScreen.main.brightness
        UIScreen.main.brightness = 0.05
        withAnimation { dimmed = true }
    }

    private func undim() {
        UIScreen.main.brightness = previousBrightness
        withAnimation { dimmed = false }
        touched()
    }

    private var dimOverlay: some View {
        ZStack {
            // Near-black overlay: real battery savings on OLED, and the
            // brightness drop covers LCDs.
            Color.black.opacity(0.96).ignoresSafeArea()
            VStack(spacing: 8) {
                Image(systemName: "video.fill")
                    .foregroundColor(.green.opacity(0.6))
                Text("Streaming — tap to wake")
                    .font(.footnote)
                    .foregroundColor(.gray)
            }
        }
        .contentShape(Rectangle())
        .onTapGesture { undim() }
    }

    private var statusBar: some View {
        HStack {
            HStack(spacing: 6) {
                Circle()
                    .fill(streamer.status == .streaming ? Color.green : Color.yellow)
                    .frame(width: 8, height: 8)
                Text(streamer.status == .streaming ? "Live" : streamer.status.label)
                    .font(.footnote.bold())
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .background(.black.opacity(0.5), in: Capsule())

            Spacer()

            Button {
                touched()
                dim()
            } label: {
                Image(systemName: "moon.fill")
                    .padding(10)
                    .background(.black.opacity(0.5), in: Circle())
            }

            Button(role: .destructive) {
                streamer.stop()
            } label: {
                Image(systemName: "stop.fill")
                    .padding(10)
                    .background(.red.opacity(0.8), in: Circle())
            }
        }
        .foregroundColor(.white)
    }

    private var controlPanel: some View {
        VStack(spacing: 12) {
            HStack {
                Image(systemName: "minus.magnifyingglass")
                Slider(value: $streamer.zoom,
                       in: 1...max(streamer.camera.maxZoomFactor, 1.1)) { editing in
                    if editing { touched() }
                }
                Image(systemName: "plus.magnifyingglass")
                Text(String(format: "%.1f×", streamer.zoom))
                    .font(.caption.monospacedDigit())
                    .frame(width: 40)
            }

            HStack {
                Image(systemName: "sun.min")
                Slider(value: $streamer.exposureBias,
                       in: streamer.camera.exposureBiasRange) { editing in
                    if editing { touched() }
                }
                Image(systemName: "sun.max")
                Text(String(format: "%+.1f", streamer.exposureBias))
                    .font(.caption.monospacedDigit())
                    .frame(width: 40)
            }

            HStack(spacing: 10) {
                Picker("Focus", selection: $streamer.focusSetting) {
                    Text("AF").tag(Streamer.FocusSetting.auto)
                    Text("Lock").tag(Streamer.FocusSetting.locked)
                }
                .pickerStyle(.segmented)
                .frame(width: 120)
                .onChange(of: streamer.focusSetting) { _ in touched() }

                if streamer.focusSetting == .locked {
                    Slider(value: $streamer.lensPosition, in: 0...1) { editing in
                        if editing { touched() }
                    }
                } else {
                    Text("Tap the preview to focus")
                        .font(.caption)
                        .foregroundColor(.gray)
                    Spacer()
                }

                if streamer.camera.hasTorch {
                    Button {
                        touched()
                        streamer.torchOn.toggle()
                    } label: {
                        Image(systemName: streamer.torchOn ? "bolt.fill" : "bolt.slash")
                            .padding(8)
                            .background(.black.opacity(0.5), in: Circle())
                    }
                }

                Menu {
                    ForEach(streamer.availableLenses) { lens in
                        Button {
                            touched()
                            streamer.selectedLens = lens
                        } label: {
                            if lens == streamer.selectedLens {
                                Label(lens.label, systemImage: "checkmark")
                            } else {
                                Text(lens.label)
                            }
                        }
                    }
                } label: {
                    Image(systemName: "camera.aperture")
                        .padding(8)
                        .background(.black.opacity(0.5), in: Circle())
                }

                Button {
                    touched()
                    streamer.flipCamera()
                } label: {
                    Image(systemName: "arrow.triangle.2.circlepath.camera")
                        .padding(8)
                        .background(.black.opacity(0.5), in: Circle())
                }
            }
        }
        .padding(14)
        .background(.black.opacity(0.55), in: RoundedRectangle(cornerRadius: 16))
        .foregroundColor(.white)
    }
}
