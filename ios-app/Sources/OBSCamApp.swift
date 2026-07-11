import SwiftUI

@main
struct OBSCamApp: App {
    @StateObject private var streamer = Streamer()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(streamer)
        }
        .onChange(of: scenePhase) { phase in
            // The camera can't capture in the background; stop cleanly so
            // OBS shows a blank source instead of a frozen frame.
            if phase == .background {
                streamer.stop()
            }
        }
    }
}
