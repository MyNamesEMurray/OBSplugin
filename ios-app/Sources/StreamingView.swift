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
        HStack(spacing: Theme.Space.m) {
            HStack(spacing: Theme.Space.s) {
                Circle()
                    .fill(streamer.status.tint)
                    .frame(width: 8, height: 8)
                Text(streamer.status.displayName)
                    .font(.footnote.bold())
                    .lineLimit(1)
            }
            .glassPill()

            Spacer()

            ControlButton(systemImage: "moon.fill") {
                touched()
                dim()
            }

            Button {
                streamer.stop()
            } label: {
                Image(systemName: "stop.fill")
                    .font(.system(size: 18, weight: .medium))
                    .frame(width: Theme.controlButton, height: Theme.controlButton)
                    .background(Theme.errorRed.opacity(0.9), in: Circle())
            }
        }
        .foregroundColor(Theme.textPrimary)
    }

    private var controlPanel: some View {
        VStack(spacing: Theme.Space.m) {
            sliderRow(minIcon: "minus.magnifyingglass",
                      maxIcon: "plus.magnifyingglass",
                      value: $streamer.zoom,
                      range: 1...max(streamer.camera.maxZoomFactor, 1.1),
                      readout: String(format: "%.1f×", streamer.zoom))

            sliderRow(minIcon: "sun.min",
                      maxIcon: "sun.max",
                      value: floatBinding($streamer.exposureBias),
                      range: exposureRange,
                      readout: String(format: "%+.1f", streamer.exposureBias))

            HStack(spacing: Theme.Space.m) {
                Picker("Focus", selection: $streamer.focusSetting) {
                    Text("AF").tag(Streamer.FocusSetting.auto)
                    Text("Lock").tag(Streamer.FocusSetting.locked)
                }
                .pickerStyle(.segmented)
                .frame(width: 120)
                .onChange(of: streamer.focusSetting) { _ in touched() }

                if streamer.focusSetting == .locked {
                    Slider(value: floatBinding($streamer.lensPosition),
                           in: 0...1) { editing in
                        if editing { touched() }
                    }
                } else {
                    Text("Tap to focus")
                        .font(.caption)
                        .foregroundColor(Theme.textSecondary)
                    Spacer()
                }

                if streamer.camera.hasTorch {
                    ControlButton(systemImage: streamer.torchOn
                                    ? "bolt.fill" : "bolt.slash",
                                  active: streamer.torchOn) {
                        touched()
                        streamer.torchOn.toggle()
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
                        .font(.system(size: 18, weight: .medium))
                        .foregroundColor(Theme.textPrimary)
                        .frame(width: Theme.controlButton,
                               height: Theme.controlButton)
                        .background(Theme.glassChip, in: Circle())
                }

                ControlButton(
                    systemImage: "arrow.triangle.2.circlepath.camera") {
                    touched()
                    streamer.flipCamera()
                }
            }
        }
        .tint(Theme.accent)
        .glassPanel()
        .foregroundColor(Theme.textPrimary)
    }

    /// Shared zoom/exposure slider row: leading icon · slider · trailing
    /// icon · monospaced readout (docs/UI_DESIGN.md §4).
    private func sliderRow(minIcon: String, maxIcon: String,
                           value: Binding<CGFloat>,
                           range: ClosedRange<CGFloat>,
                           readout: String) -> some View {
        HStack(spacing: Theme.Space.m) {
            Image(systemName: minIcon)
            Slider(value: value, in: range) { editing in
                if editing { touched() }
            }
            Image(systemName: maxIcon)
            Text(readout)
                .font(.caption.monospacedDigit())
                .frame(width: 44, alignment: .trailing)
        }
    }

    private var exposureRange: ClosedRange<CGFloat> {
        let r = streamer.camera.exposureBiasRange
        return CGFloat(r.lowerBound)...CGFloat(r.upperBound)
    }

    /// Bridges a Float model value to the CGFloat sliders.
    private func floatBinding(_ source: Binding<Float>) -> Binding<CGFloat> {
        Binding(get: { CGFloat(source.wrappedValue) },
                set: { source.wrappedValue = Float($0) })
    }
}
