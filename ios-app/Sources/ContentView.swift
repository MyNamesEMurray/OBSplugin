import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var streamer: Streamer

    var body: some View {
        NavigationView {
            Form {
                previewSection
                statusSection

                if !streamer.isStreaming {
                    connectionSection
                    cameraSection
                }

                actionSection
            }
            .navigationTitle("OBSCam")
            .onAppear { streamer.refreshServers() }
        }
        .navigationViewStyle(.stack)
    }

    private var previewSection: some View {
        Section {
            CameraPreviewView(session: streamer.camera.session)
                .aspectRatio(16 / 9, contentMode: .fit)
                .frame(maxWidth: .infinity)
                .background(Color.black)
                .cornerRadius(8)
                .listRowInsets(EdgeInsets())
                .overlay {
                    if !streamer.isStreaming {
                        Text("Preview starts with streaming")
                            .font(.caption)
                            .foregroundColor(.white.opacity(0.7))
                    }
                }
        }
    }

    private var statusSection: some View {
        Section {
            HStack {
                Circle()
                    .fill(statusColor)
                    .frame(width: 10, height: 10)
                Text(streamer.status.label)
                    .font(.callout)
            }
        }
    }

    private var statusColor: Color {
        switch streamer.status {
        case .idle: return .gray
        case .connecting: return .yellow
        case .streaming: return .green
        case .error: return .red
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

    private var cameraSection: some View {
        Section("Camera") {
            Toggle("Front camera", isOn: $streamer.useFrontCamera)

            Picker("Resolution", selection: $streamer.resolution) {
                ForEach(CameraManager.Resolution.allCases) { resolution in
                    Text(resolution.rawValue).tag(resolution)
                }
            }

            Picker("Frame rate", selection: $streamer.fps) {
                Text("30 fps").tag(30)
                Text("60 fps").tag(60)
            }
        }
    }

    private var actionSection: some View {
        Section {
            if streamer.isStreaming {
                Button(role: .destructive) {
                    streamer.stop()
                } label: {
                    Label("Stop", systemImage: "stop.fill")
                        .frame(maxWidth: .infinity)
                }
            } else {
                Button {
                    Task { await streamer.start() }
                } label: {
                    Label("Start streaming to OBS", systemImage: "video.fill")
                        .frame(maxWidth: .infinity)
                }
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
