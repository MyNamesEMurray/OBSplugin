import SwiftUI

/// The Options sheet: the behaviour toggles, each with its own short
/// explanation. They live off the main screen so the Setup form stays
/// compact — a single wall-of-text footer under four toggles was
/// unreadable, and pushed the form into scrolling.
struct OptionsView: View {
    @EnvironmentObject private var streamer: Streamer
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationView {
            Form {
                Section {
                    Toggle("Remote start from OBS",
                           isOn: $streamer.remoteStartEnabled)
                } footer: {
                    Text("While the app is open and idle, OBS can start the camera for you — automatically when its source connects, or from the source's \"Start camera on the phone\" button. Siri works too: \"Start streaming with LensLink.\"")
                }

                Section {
                    Toggle("Dim screen while streaming",
                           isOn: $streamer.dimWhileStreaming)
                } footer: {
                    Text("The screen dims after 10 seconds of streaming to save battery; tap it to wake.")
                }

                Section {
                    Toggle("Send phone mic to OBS",
                           isOn: $streamer.sendMicAudio)
                    Toggle("Auto lip-sync reference",
                           isOn: $streamer.sendAudioReference)
                } header: {
                    Text("Microphone")
                } footer: {
                    // One capture, two jobs — Streamer enforces the
                    // exclusivity; this footer is where users learn it.
                    Text("**Send phone mic** streams this phone's microphone as the camera source's audio in OBS — a wireless mic.\n\n**Auto lip-sync** sends the mic purely as a timing reference so the plugin can auto-align your real microphone — it's never streamed or heard.\n\nOne mic, one role: turning one on turns the other off.")
                }
            }
            .navigationTitle("Options")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .navigationViewStyle(.stack)
        .tint(Theme.accent)
    }
}
