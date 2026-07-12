import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var streamer: Streamer

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
                modeSection
                if streamer.connectionMode == .dial {
                    connectionSection
                } else {
                    receiveSection
                }
                cameraSection
                optionsSection
                actionSection
            }
            .navigationTitle("OBSCam")
            .onAppear { streamer.refreshServers() }
        }
        .navigationViewStyle(.stack)
    }

    private var optionsSection: some View {
        Section {
            Toggle("Dim screen while streaming", isOn: $streamer.dimWhileStreaming)
        } footer: {
            Text("Saves battery: the screen dims after 10 seconds of streaming; tap to wake it.")
        }
    }

    private var statusSection: some View {
        Section {
            HStack {
                Circle()
                    .fill(statusColor)
                    .frame(width: 10, height: 10)
                Text(statusText)
                    .font(.callout)
            }
        }
    }

    private var statusText: String {
        if streamer.status == .connecting && streamer.connectionMode == .receive {
            return "Waiting for OBS to connect…"
        }
        return streamer.status.label
    }

    private var statusColor: Color {
        switch streamer.status {
        case .idle: return .gray
        case .connecting: return .yellow
        case .streaming: return .green
        case .error: return .red
        }
    }

    private var modeSection: some View {
        Section {
            Picker("Connection", selection: $streamer.connectionMode) {
                ForEach(Streamer.ConnectionMode.allCases) { mode in
                    Text(mode.label).tag(mode)
                }
            }
            .pickerStyle(.segmented)
        }
    }

    private var receiveSection: some View {
        Section("How to connect") {
            if let ip = NetworkInfo.wifiIPAddress() {
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
                Text("In OBS, add an \"iOS Camera\" source and enter this address as the Phone IP.")
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

    private var connectionSection: some View {
        Section("OBS Connection") {
            if !streamer.discoveredServers.isEmpty {
                ForEach(streamer.discoveredServers) { server in
                    Button {
                        streamer.select(server)
                    } label: {
                        HStack {
                            Image(systemName: "desktopcomputer")
                            VStack(alignment: .leading) {
                                Text(server.name)
                                Text("\(server.host):\(String(server.port))")
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                            }
                            Spacer()
                            if streamer.host == server.host {
                                Image(systemName: "checkmark")
                                    .foregroundColor(.accentColor)
                            }
                        }
                    }
                    .foregroundColor(.primary)
                }
            }

            TextField("Computer IP (e.g. 192.168.1.20)", text: $streamer.host)
                .keyboardType(.numbersAndPunctuation)
                .autocorrectionDisabled()
                .textInputAutocapitalization(.never)

            TextField("Port", text: $streamer.portText)
                .keyboardType(.numberPad)

            Button {
                streamer.refreshServers()
            } label: {
                Label("Scan for OBS on this network", systemImage: "arrow.clockwise")
            }
        }
    }

    /// Only resolutions this lens actually supports (at any frame rate).
    private var availableResolutions: [CameraManager.Resolution] {
        CameraManager.Resolution.allCases.filter {
            CameraManager.supports(resolution: $0, fps: 30,
                                   lens: streamer.selectedLens)
        }
    }

    /// Only frame rates this lens supports at the chosen resolution.
    private var availableFrameRates: [Int] {
        [30, 60].filter {
            CameraManager.supports(resolution: streamer.resolution,
                                   fps: Int32($0),
                                   lens: streamer.selectedLens)
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
            .onChange(of: streamer.resolution) { _ in
                streamer.clampCaptureSettings()
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
                    .frame(maxWidth: .infinity)
            }

            if streamer.cameraPermissionDenied {
                Button("Open Settings") {
                    if let url = URL(string: UIApplication.openSettingsURLString) {
                        UIApplication.shared.open(url)
                    }
                }
            }
        }
    }
}
