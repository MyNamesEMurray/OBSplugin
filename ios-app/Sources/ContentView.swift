import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var streamer: Streamer

    // Cached per change, not per render: a Form body re-evaluates on any
    // published change, and these hit AVCaptureDevice discovery/format
    // scans (capability checks) or getifaddrs (the IP) each time.
    @State private var wifiIP: String?
    @State private var availableResolutions: [CameraManager.Resolution] = []
    @State private var availableFrameRates: [Int] = []

    var body: some View {
        if streamer.isStreaming {
            StreamingView()
        } else {
            settingsForm
        }
    }

    private var settingsForm: some View {
        NavigationView {
            Form {
                statusSection
                receiveSection
                cameraSection
                optionsSection
                lipSyncSection
                actionSection
                screenMirrorSection
            }
            .navigationTitle("LensLink")
        }
        .navigationViewStyle(.stack)
        .tint(Theme.accent)
        .onAppear {
            wifiIP = NetworkInfo.wifiIPAddress()
            refreshCapabilities()
        }
        .onReceive(NotificationCenter.default.publisher(
            for: UIApplication.willEnterForegroundNotification)) { _ in
            wifiIP = NetworkInfo.wifiIPAddress()
        }
        .onChange(of: streamer.selectedLens) { _ in refreshCapabilities() }
        .onChange(of: streamer.resolution) { _ in
            streamer.clampCaptureSettings()
            refreshCapabilities()
        }
    }

    private func refreshCapabilities() {
        availableResolutions = CameraManager.Resolution.allCases.filter {
            CameraManager.supports(resolution: $0, fps: 30,
                                   lens: streamer.selectedLens)
        }
        availableFrameRates = [30, 60].filter {
            CameraManager.supports(resolution: streamer.resolution,
                                   fps: Int32($0),
                                   lens: streamer.selectedLens)
        }
    }

    private var optionsSection: some View {
        Section {
            Toggle("Dim screen while streaming", isOn: $streamer.dimWhileStreaming)
        } footer: {
            Text("Saves battery: the screen dims after 10 seconds of streaming; tap to wake it.")
        }
    }

    private var lipSyncSection: some View {
        Section {
            Toggle("Auto lip-sync reference", isOn: $streamer.sendAudioReference)
        } footer: {
            Text("Sends the phone mic to OBS purely as a timing reference so the plugin can auto-align your real microphone to the video. Your phone audio is never streamed or heard.")
        }
    }

    /// Screen mirroring is a separate path (a broadcast extension), not the
    /// camera pipeline — so it lives in its own section with the system
    /// broadcast picker.
    private var screenMirrorSection: some View {
        Section("Mirror your screen instead") {
            HStack(spacing: Theme.Space.m) {
                BroadcastButton()
                    .frame(width: 52, height: 52)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Screen mirror to OBS")
                        .font(.body.weight(.semibold))
                    Text("Streams your whole screen + app audio. Point the same OBS source at this phone.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }
        }
    }

    private var statusSection: some View {
        Section {
            HStack(spacing: Theme.Space.m) {
                Circle()
                    .fill(streamer.status.tint)
                    .frame(width: 10, height: 10)
                Text(streamer.status.displayName)
                    .font(.callout)
            }
        }
    }

    private var receiveSection: some View {
        Section("How to connect") {
            if let ip = wifiIP {
                HStack {
                    Image(systemName: "wifi")
                    VStack(alignment: .leading, spacing: 2) {
                        Text("This phone's address")
                            .font(.caption)
                            .foregroundColor(.secondary)
                        Text(ip)
                            .font(.title3.monospacedDigit().bold())
                            .textSelection(.enabled)
                    }
                }
                Text("In OBS, add a \"LensLink Camera\" source and enter this address as the Phone IP.")
                    .font(.callout)
                    .foregroundColor(.secondary)
            } else {
                Text("No Wi-Fi address found — connect to Wi-Fi, or use a USB cable.")
                    .font(.callout)
                    .foregroundColor(.secondary)
            }
            Label {
                Text("Or plug in a USB cable and set the OBS source's Connection to \"USB cable\" (needs iTunes on Windows). No Wi-Fi required.")
                    .font(.callout)
                    .foregroundColor(.secondary)
            } icon: {
                Image(systemName: "cable.connector")
            }
        }
    }

    private var cameraSection: some View {
        Section("Camera") {
            Picker("Lens", selection: $streamer.selectedLens) {
                ForEach(streamer.availableLenses) { lens in
                    Text(lens.label).tag(lens)
                }
            }

            Picker("Resolution", selection: $streamer.resolution) {
                ForEach(availableResolutions) { resolution in
                    Text(resolution.rawValue).tag(resolution)
                }
            }

            Picker("Frame rate", selection: $streamer.fps) {
                ForEach(availableFrameRates, id: \.self) { fps in
                    Text("\(fps) fps").tag(fps)
                }
            }

            Picker("Codec", selection: $streamer.codec) {
                Text(VideoCodec.h264.label).tag(VideoCodec.h264)
                if VideoEncoder.isSupported(.hevc) {
                    Text(VideoCodec.hevc.label).tag(VideoCodec.hevc)
                }
            }
        }
    }

    private var actionSection: some View {
        Section {
            Button {
                Task { await streamer.start() }
            } label: {
                Label("Start streaming to OBS", systemImage: "video.fill")
                    .font(.body.weight(.semibold))
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .tint(Theme.accent)
            .listRowInsets(EdgeInsets())
            .listRowBackground(Color.clear)

            if streamer.cameraPermissionDenied || streamer.micPermissionDenied {
                Button("Open Settings") {
                    if let url = URL(string: UIApplication.openSettingsURLString) {
                        UIApplication.shared.open(url)
                    }
                }
            }
        }
    }
}
