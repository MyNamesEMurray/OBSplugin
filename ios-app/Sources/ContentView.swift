import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var streamer: Streamer

    // Cached per change, not per render: a Form body re-evaluates on any
    // published change, and these hit AVCaptureDevice discovery/format
    // scans (capability checks) or getifaddrs (the IP) each time.
    @State private var wifiIP: String?
    @State private var availableResolutions: [CameraManager.Resolution] = []
    @State private var availableFrameRates: [Int] = []

    // Collapsible help: the connection instructions are essential exactly
    // once. Persisted so the form stays minimal after the user has
    // collapsed them, keeping Start/Mirror near the top of the screen.
    @AppStorage("showConnectionHelp") private var showConnectionHelp = true
    @AppStorage("showMirrorTools") private var showMirrorTools = false

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
                connectSection
                streamSection
                cameraSection
                optionsSection
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

    /// Status + the phone's address on one compact line; the longer
    /// instructions collapse away once read.
    private var connectSection: some View {
        Section {
            HStack(spacing: Theme.Space.m) {
                Circle()
                    .fill(streamer.status.tint)
                    .frame(width: 10, height: 10)
                Text(streamer.status.displayName)
                    .font(.callout)
                Spacer()
                if let ip = wifiIP {
                    Text(ip)
                        .font(.callout.monospacedDigit().bold())
                        .textSelection(.enabled)
                }
            }
            DisclosureGroup("How to connect", isExpanded: $showConnectionHelp) {
                if let ip = wifiIP {
                    Label {
                        Text("In OBS, add a \"LensLink Camera\" source and enter \(Text(ip).bold()) as the Phone IP. (Phone and computer must be on the same Wi-Fi.)")
                            .font(.callout)
                            .foregroundColor(.secondary)
                    } icon: {
                        Image(systemName: "wifi")
                    }
                } else {
                    Label {
                        Text("No Wi-Fi address found — connect to Wi-Fi, or use a USB cable.")
                            .font(.callout)
                            .foregroundColor(.secondary)
                    } icon: {
                        Image(systemName: "wifi.slash")
                    }
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
    }

    @State private var probeResult: String?
    @State private var extensionStatus = ""

    /// Both ways to stream, side by side near the top: the camera (the
    /// primary action) and the screen mirror (a broadcast extension, so it
    /// uses the system picker). Mirror diagnostics hide in a disclosure.
    private var streamSection: some View {
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

            HStack(spacing: Theme.Space.m) {
                BroadcastButton()
                    .frame(width: 44, height: 44)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Mirror your screen instead")
                        .font(.body.weight(.semibold))
                    Text("Whole screen + app audio, to a \"LensLink Screen\" source.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }

            // Surface a broken extension unconditionally (sideloading can
            // silently drop it); the healthy checkmark stays collapsed.
            if !extensionStatus.isEmpty && !extensionStatus.hasPrefix("✓") {
                Text(extensionStatus)
                    .font(.caption)
                    .foregroundColor(.red)
            }

            DisclosureGroup("Screen mirror tools", isExpanded: $showMirrorTools) {
                Text(extensionStatus)
                    .font(.caption)
                    .foregroundColor(.secondary)
                // Diagnostic: verifies the broadcast extension's listener
                // is reachable on-device, independent of OBS/USB. Run it
                // while a broadcast is active.
                Button {
                    probeResult = "Checking…"
                    BroadcastProbe.run { ok in
                        probeResult = ok
                            ? "✓ Broadcast link is up — OBS should be able to connect"
                            : "✗ No listener — is a screen broadcast running? If yes, the extension isn't working"
                    }
                } label: {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Check broadcast link")
                        if let probeResult {
                            Text(probeResult)
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }
                    }
                }
            }
        }
        .onAppear {
            // Whether the extension survived sideloading — the broadcast
            // picker can show a stale entry even when it didn't.
            extensionStatus = BroadcastProbe.installedExtensionDescription()
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

    private var optionsSection: some View {
        // Section has no (title-string + footer-closure) initializer; the
        // header must be a closure too.
        Section {
            Toggle("Dim screen while streaming", isOn: $streamer.dimWhileStreaming)
            Toggle("Auto lip-sync reference", isOn: $streamer.sendAudioReference)
        } header: {
            Text("Options")
        } footer: {
            Text("Dim: the screen dims after 10 seconds of streaming; tap to wake. Lip-sync: sends the phone mic to OBS purely as a timing reference so the plugin can auto-align your real microphone — it is never streamed or heard.")
        }
    }
}
