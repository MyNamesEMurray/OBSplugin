import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var streamer: Streamer

    // Cached per change, not per render: a Form body re-evaluates on any
    // published change, and these hit AVCaptureDevice discovery/format
    // scans (capability checks) or getifaddrs (the IP) each time.
    @State private var wifiIP: String?
    @State private var availableResolutions: [CameraManager.Resolution] = []
    @State private var availableFrameRates: [Int] = []

    // Collapsible extras: essential exactly once, then noise. Persisted so
    // the form stays compact after the user has read them.
    @AppStorage("showConnectionHelp") private var showConnectionHelp = true
    @AppStorage("showMirrorTools") private var showMirrorTools = false

    var body: some View {
        if streamer.isStreaming {
            StreamingView()
        } else {
            settingsForm
        }
    }

    // The form is three parallel modules — Connect, Camera, Screen mirror —
    // each saying which OBS source it talks to and ending in the same
    // full-width action button, plus small Options/About tails. The banner
    // is the title (no NavigationView: nothing is ever pushed, and the
    // wordmark replaces the large-title text).
    private var settingsForm: some View {
        Form {
            bannerHeader
            connectSection
            cameraSection
            screenMirrorSection
            optionsSection
            aboutSection
        }
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

    // MARK: - Banner

    /// The wordmark as the screen's title. Light/dark variants switch
    /// automatically via the asset catalog's luminosity appearances.
    private var bannerHeader: some View {
        Section {
            Image("Banner")
                .resizable()
                .scaledToFit()
                .frame(height: 48)
                .frame(maxWidth: .infinity)
                .accessibilityLabel("LensLink")
                .listRowBackground(Color.clear)
                .listRowInsets(EdgeInsets())
                .padding(.top, Theme.Space.s)
        }
    }

    // MARK: - Connect

    /// Status + the phone's address on one line; setup instructions
    /// collapse away once read.
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
                Label {
                    Text("Install the LensLink plugin in OBS Studio (see the GitHub link below), then add the source you want — camera or screen — from **Sources → +**.")
                        .font(.callout)
                        .foregroundColor(.secondary)
                } icon: {
                    Image(systemName: "1.circle")
                }
                Label {
                    if let ip = wifiIP {
                        Text("Point it at this phone: enter \(Text(ip).bold()) as the Phone IP (same Wi-Fi), or plug in a USB cable and set Connection to \"USB cable\" (Windows needs iTunes).")
                            .font(.callout)
                            .foregroundColor(.secondary)
                    } else {
                        Text("No Wi-Fi address found — connect to Wi-Fi, or plug in a USB cable and set the source's Connection to \"USB cable\" (Windows needs iTunes).")
                            .font(.callout)
                            .foregroundColor(.secondary)
                    }
                } icon: {
                    Image(systemName: "2.circle")
                }
            }
        } header: {
            Text("Connect")
        }
    }

    // MARK: - Camera

    private var cameraSection: some View {
        Section {
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

            if streamer.cameraPermissionDenied || streamer.micPermissionDenied {
                Button("Camera access denied — open Settings") {
                    if let url = URL(string: UIApplication.openSettingsURLString) {
                        UIApplication.shared.open(url)
                    }
                }
            }

            Button {
                Task { await streamer.start() }
            } label: {
                ActionRowLabel(title: "Start camera stream",
                               systemImage: "video.fill")
            }
            .buttonStyle(.plain)
            .listRowInsets(EdgeInsets())
            .listRowBackground(Color.clear)
        } header: {
            Text("Camera")
        } footer: {
            Text("Streams to a \"LensLink Camera\" source in OBS.")
        }
    }

    // MARK: - Screen mirror

    @State private var probeResult: String?
    @State private var extensionStatus = ""

    /// Same shape as the camera module: content, then one full-width
    /// action button. The button face is ours; the (invisible) system
    /// broadcast picker stretched over it receives the tap, because iOS
    /// won't start a broadcast any other way.
    private var screenMirrorSection: some View {
        Section {
            // Surface a broken extension unconditionally (sideloading can
            // silently drop it); the healthy checkmark lives in the tools
            // disclosure below.
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
                    BroadcastProbe.run { result in
                        switch result {
                        case .screenListener:
                            probeResult = "✓ Broadcast link is up — OBS should be able to connect"
                        case .appListener:
                            probeResult = "✗ Only the app's own listener answered — start a screen broadcast, then run this again"
                        case .none:
                            probeResult = "✗ No listener — is a screen broadcast running? If yes, the extension isn't working"
                        }
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

            ZStack {
                ActionRowLabel(title: "Start screen broadcast",
                               systemImage: "rectangle.on.rectangle")
                BroadcastPickerOverlay()
            }
            .listRowInsets(EdgeInsets())
            .listRowBackground(Color.clear)
        } header: {
            Text("Screen mirror")
        } footer: {
            Text("Streams your whole screen, with app audio, to a \"LensLink Screen\" source in OBS. DRM audio (Apple Music, Netflix) is muted by iOS during broadcasts.")
        }
        .onAppear {
            // Whether the extension survived sideloading — the broadcast
            // picker can show a stale entry even when it didn't.
            extensionStatus = BroadcastProbe.installedExtensionDescription()
        }
    }

    // MARK: - Options / About

    private var optionsSection: some View {
        // Section has no (title-string + footer-closure) initializer; the
        // header must be a closure too.
        Section {
            Toggle("Remote start from OBS", isOn: $streamer.remoteStartEnabled)
            Toggle("Dim screen while streaming", isOn: $streamer.dimWhileStreaming)
            Toggle("Send phone mic to OBS", isOn: $streamer.sendMicAudio)
            Toggle("Auto lip-sync reference", isOn: $streamer.sendAudioReference)
        } header: {
            Text("Options")
        } footer: {
            Text("Remote start: while LensLink is open, OBS can start the camera for you — automatically when its source connects, or from the source's \"Start camera on the phone\" button. Siri works too: \"Start streaming with LensLink.\" Dim: the screen dims after 10 seconds of streaming; tap to wake. Phone mic: streams this phone's microphone as the camera source's audio in OBS — a wireless mic. Lip-sync: sends the phone mic purely as a timing reference so the plugin can auto-align your real microphone — never streamed or heard. The mic toggles are either/or: one mic, one role.")
        }
    }

    /// The OBS plugin, docs, and issue tracker all live on GitHub —
    /// TestFlight testers otherwise have no pointer to them. The version
    /// line gives bug reports a build to cite.
    private var aboutSection: some View {
        Section {
            Link(destination: URL(string: "https://github.com/MyNamesEMurray/LensLink")!) {
                Label("LensLink on GitHub", systemImage: "link")
            }
        } footer: {
            Text("OBS plugin downloads, guides, and bug reports. \(Self.versionLine)")
        }
    }

    private static let versionLine: String = {
        let info = Bundle.main.infoDictionary
        let version = info?["CFBundleShortVersionString"] as? String ?? "?"
        let build = info?["CFBundleVersion"] as? String ?? "?"
        return "LensLink v\(version) (\(build))"
    }()
}

/// The one action-button face used by every module, so "Start camera
/// stream" and "Start screen broadcast" read as the same kind of control.
private struct ActionRowLabel: View {
    let title: String
    let systemImage: String

    var body: some View {
        Label(title, systemImage: systemImage)
            .font(.body.weight(.semibold))
            .foregroundColor(.white)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 14)
            .background(Theme.accent,
                        in: RoundedRectangle(cornerRadius: Theme.Radius.chip))
    }
}
