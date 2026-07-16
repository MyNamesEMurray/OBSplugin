import Foundation
import AVFoundation
import Combine
import SwiftUI

/// Glues camera → encoder → network together and exposes state for SwiftUI.
@MainActor
final class Streamer: ObservableObject {
    enum Status: Equatable {
        case idle
        /// Remote start: OBS is connected and can start the camera from
        /// the computer; the camera itself isn't running yet.
        case standby
        case connecting
        case streaming
        case error(String)

        /// The single canonical status word/phrase used by every surface
        /// (see docs/UI_DESIGN.md §2).
        var displayName: String {
            switch self {
            case .idle: return "Not connected"
            case .standby: return "OBS connected — ready"
            case .connecting: return "Waiting for OBS…"
            case .streaming: return "Live"
            case .error(let message): return message
            }
        }

        /// The status dot/label colour from the shared palette.
        var tint: Color {
            switch self {
            case .idle: return Theme.idleGrey
            case .standby: return Theme.connectAmber
            case .connecting: return Theme.connectAmber
            case .streaming: return Theme.liveGreen
            case .error: return Theme.errorRed
            }
        }
    }

    /// Shared instance: the SwiftUI scene, the Siri App Intents, and the
    /// lenslink:// URL handler must all drive the same streamer.
    static let shared = Streamer()

    // Settings (persisted to UserDefaults)
    @Published var resolution: CameraManager.Resolution {
        didSet { UserDefaults.standard.set(resolution.rawValue, forKey: "resolution") }
    }
    @Published var fps: Int {
        didSet { UserDefaults.standard.set(fps, forKey: "fps") }
    }
    /// The cameras this device actually has (Main / Ultra Wide / …).
    let availableLenses: [CameraManager.Lens]

    @Published var selectedLens: CameraManager.Lens {
        didSet {
            UserDefaults.standard.set(selectedLens.id, forKey: "selectedLens")
            let oldResolution = resolution
            let oldFps = fps
            clampCaptureSettings()
            // Live lens switch: reconfigure capture under the running
            // connection. If clamping changed resolution/fps, the encoder
            // (fixed dimensions) must be rebuilt and OBS re-configured —
            // feeding differently-sized buffers into the old session
            // breaks the stream.
            if isStreaming {
                reconfigureLiveCapture(
                    formatChanged: resolution != oldResolution
                        || fps != oldFps)
            }
        }
    }

    private func reconfigureLiveCapture(formatChanged: Bool) {
        do {
            try camera.configure(lens: selectedLens,
                                 resolution: resolution,
                                 fps: Int32(fps))
        } catch {
            // Never swallow this: a failed reconfigure leaves the session
            // without input/output — a black stream labelled "Live".
            status = .error(error.localizedDescription)
            stopKeepingError()
            return
        }
        resetCameraControls()

        // Same HEVC fallback as start(): the codec setting may have been
        // switched (remotely or locally) to one this device can't encode.
        let activeCodec = (codec == .hevc && !VideoEncoder.isSupported(.hevc))
            ? VideoCodec.h264 : codec
        if let oldEncoder = encoder,
           formatChanged || oldEncoder.codec != activeCodec {
            rebuildEncoder(codec: activeCodec)
        }

        encoder?.requestKeyframe()
        scheduleStateSend()
    }

    /// Tears down the running encoder and builds a fresh one for the
    /// current capture size (fixed-dimension encoders can't follow a
    /// resolution/orientation change), re-announcing the format to OBS.
    private func rebuildEncoder(codec activeCodec: VideoCodec) {
        guard let oldEncoder = encoder else { return }
        oldEncoder.stop()
        let size = resolution.size
        let newEncoder = VideoEncoder(
            codec: activeCodec,
            width: size.width, height: size.height,
            fps: Int32(fps),
            bitrate: resolution.bitrate(for: activeCodec))
        do {
            try newEncoder.start()
        } catch {
            status = .error(error.localizedDescription)
            stopKeepingError()
            return
        }
        newEncoder.onEncodedFrame = { [weak self] frame in
            self?.client.sendVideoFrame(frame)
        }
        camera.onSampleBuffer = { [weak newEncoder] sampleBuffer in
            newEncoder?.encode(sampleBuffer)
        }
        encoder = newEncoder
        client.sendVideoConfig(codec: activeCodec,
                               width: size.width, height: size.height,
                               fps: Int32(fps))
        startAdaptiveBitrate(
            target: resolution.bitrate(for: activeCodec))
    }
    @Published var codec: VideoCodec {
        didSet { UserDefaults.standard.set(codec.rawValue, forKey: "videoCodec") }
    }
    @Published var dimWhileStreaming: Bool {
        didSet { UserDefaults.standard.set(dimWhileStreaming, forKey: "dimWhileStreaming") }
    }
    /// Send phone-mic audio as a lip-sync calibration reference (never
    /// played out — the plugin correlates it against your real mic).
    /// Mutually exclusive with `sendMicAudio` — one mic, one role.
    @Published var sendAudioReference: Bool {
        didSet {
            UserDefaults.standard.set(sendAudioReference, forKey: "sendAudioReference")
            if sendAudioReference && sendMicAudio {
                sendMicAudio = false
            }
        }
    }
    /// Send phone-mic audio as the OBS source's *playable* audio — the
    /// phone as a wireless microphone.
    @Published var sendMicAudio: Bool {
        didSet {
            UserDefaults.standard.set(sendMicAudio, forKey: "sendMicAudio")
            if sendMicAudio && sendAudioReference {
                sendAudioReference = false
            }
        }
    }
    /// Which microphone feeds the capture (an `AudioReference.MicOption`
    /// id; "auto" = system routing). Hot-switchable mid-stream, from the
    /// Live screen or the web panel's mic row.
    @Published var selectedMicID: String {
        didSet {
            UserDefaults.standard.set(selectedMicID, forKey: "selectedMic")
            audioReference?.preferredMicID = selectedMicID
            if audioReference != nil {
                AudioReference.select(micID: selectedMicID)
            }
            scheduleStateSend()
        }
    }
    /// Selectable mics right now. Live list — recomputed per access so a
    /// headset connecting mid-stream shows up on the next UI refresh.
    var micOptions: [AudioReference.MicOption] {
        AudioReference.availableMics()
    }
    /// Remote start: while the app is open and idle, keep listening so OBS
    /// can start (and stop) the camera from the computer — the plugin's
    /// auto-start, its "Start camera on the phone" button, or the web panel.
    @Published var remoteStartEnabled: Bool {
        didSet {
            UserDefaults.standard.set(remoteStartEnabled, forKey: "remoteStartEnabled")
            updateStandby()
        }
    }
    @Published var micPermissionDenied = false
    /// Whether the listener is advertising over Bonjour (nil = not
    /// listening). false = iOS denied Local Network permission and the
    /// app fell back to a plain listener — the Connect screen points at
    /// the Settings toggle so the OBS dropdown's silence is explained.
    @Published private(set) var discoverable: Bool?

    // Live camera controls (also driven remotely via CONTROL packets)
    @Published var zoom: CGFloat = 1 {
        didSet {
            camera.setZoom(zoom)
            scheduleStateSend()
        }
    }
    @Published var exposureBias: Float = 0 {
        didSet {
            camera.setExposureBias(exposureBias)
            scheduleStateSend()
        }
    }
    enum FocusSetting: Equatable {
        case auto
        case locked
    }
    @Published var focusSetting: FocusSetting = .auto {
        didSet {
            applyFocus()
            scheduleStateSend()
        }
    }
    @Published var lensPosition: Float = 0.5 {
        didSet {
            if focusSetting == .locked { applyFocus() }
            scheduleStateSend()
        }
    }
    @Published var flashlightOn: Bool = false {
        didSet {
            camera.setFlashlight(flashlightOn)
            scheduleStateSend()
        }
    }
    enum WhiteBalanceSetting: Equatable {
        case auto
        case locked
    }
    @Published var whiteBalanceSetting: WhiteBalanceSetting = .auto {
        didSet {
            applyWhiteBalance()
            scheduleStateSend()
        }
    }
    /// Colour temperature (Kelvin) used while white balance is locked.
    @Published var whiteBalanceTemperature: Float = 5000 {
        didSet {
            if whiteBalanceSetting == .locked { applyWhiteBalance() }
            scheduleStateSend()
        }
    }
    enum ExposureSetting: Equatable {
        case auto
        case manual
    }
    @Published var exposureSetting: ExposureSetting = .auto {
        didSet {
            applyExposure()
            scheduleStateSend()
        }
    }
    @Published var iso: Float = 200 {
        didSet {
            if exposureSetting == .manual { applyExposure() }
            scheduleStateSend()
        }
    }
    @Published var shutterSeconds: Double = 1.0 / 60 {
        didSet {
            if exposureSetting == .manual { applyExposure() }
            scheduleStateSend()
        }
    }

    private func applyWhiteBalance() {
        switch whiteBalanceSetting {
        case .auto:
            camera.setAutoWhiteBalance()
        case .locked:
            camera.lockWhiteBalance(temperature: whiteBalanceTemperature)
        }
    }

    private func applyExposure() {
        switch exposureSetting {
        case .auto:
            camera.setAutoExposure()
        case .manual:
            camera.setManualExposure(iso: iso, shutterSeconds: shutterSeconds)
        }
    }

    /// Tap-to-focus from the Live screen. Routed through here (not straight
    /// to the camera) so a focus tap keeps a manual ISO/shutter lock intact.
    func focusAndExpose(at devicePoint: CGPoint) {
        camera.focusAndExpose(at: devicePoint,
                              includeExposure: exposureSetting == .auto)
    }

    /// Debounced push of the control state to the plugin (for its web UI).
    private var stateSendPending = false

    private func scheduleStateSend() {
        guard isStreaming, !stateSendPending else { return }
        stateSendPending = true
        Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 200_000_000)
            guard let self else { return }
            self.stateSendPending = false
            guard self.isStreaming else { return }
            self.client.sendState(self.controlStateSnapshot())
        }
    }

    private func controlStateSnapshot() -> [String: Any] {
        let (resolutions, frameRates) = formatCapabilities()
        var state: [String: Any] = [
            "zoom": Double(zoom),
            "maxZoom": Double(camera.maxZoomFactor),
            "exposureBias": Double(exposureBias),
            "focusMode": focusSetting == .locked ? "locked" : "auto",
            "lensPosition": Double(lensPosition),
            "flashlight": flashlightOn,
            "hasFlashlight": camera.hasFlashlight,
            "camera": selectedLens.position == .front ? "front" : "back",
            "lens": selectedLens.label,
            "lenses": availableLenses.map { $0.label },
            // White balance / manual exposure (docs/PROTOCOL.md §8); the
            // supports* flags let remote UIs hide what this camera lacks.
            "whiteBalanceMode": whiteBalanceSetting == .locked ? "locked" : "auto",
            "whiteBalanceTemperature": Double(whiteBalanceTemperature),
            "supportsWhiteBalanceLock": camera.supportsWhiteBalanceLock,
            "exposureMode": exposureSetting == .manual ? "manual" : "auto",
            "iso": Double(iso),
            "minISO": Double(camera.isoRange.lowerBound),
            "maxISO": Double(camera.isoRange.upperBound),
            "shutterSeconds": shutterSeconds,
            "minShutterSeconds": camera.minShutterSeconds,
            "maxShutterSeconds": camera.maxShutterSeconds(fps: Int32(fps)),
            "supportsManualExposure": camera.supportsManualExposure,
            // Format state + what this lens supports, so remote UIs can
            // offer set_format pickers with only valid choices.
            "resolution": resolution.rawValue,
            "fps": fps,
            "codec": (encoder?.codec ?? codec).rawValue,
            "resolutions": resolutions,
            "frameRates": frameRates,
            "codecs": VideoCodec.allCases
                .filter { VideoEncoder.isSupported($0) }
                .map { $0.rawValue },
        ]
        // Mic picker (docs/PROTOCOL.md §8): only while the phone mic is
        // live as the source's audio — remote UIs key their row off
        // micEnabled, and the list is only meaningful with capture up.
        if sendMicAudio, audioReference != nil {
            state["micEnabled"] = true
            state["mic"] = selectedMicID
            state["mics"] = micOptions.map { ["id": $0.id, "name": $0.name] }
        }
        return state
    }

    /// Capability lists for the STATE snapshot. Cached per lens+resolution:
    /// `CameraManager.supports` walks the device's format table, and the
    /// snapshot is resent on every (debounced) control change — e.g. for
    /// the whole length of a zoom drag.
    private var capabilityCache: (key: String, resolutions: [String], frameRates: [Int])?

    private func formatCapabilities() -> ([String], [Int]) {
        let key = "\(selectedLens.id)|\(resolution.rawValue)"
        if let cached = capabilityCache, cached.key == key {
            return (cached.resolutions, cached.frameRates)
        }
        let resolutions = CameraManager.Resolution.allCases.filter {
            CameraManager.supports(resolution: $0, fps: 30, lens: selectedLens)
        }.map { $0.rawValue }
        let frameRates = [30, 60].filter {
            CameraManager.supports(resolution: resolution, fps: Int32($0),
                                   lens: selectedLens)
        }
        capabilityCache = (key, resolutions, frameRates)
        return (resolutions, frameRates)
    }

    private func applyFocus() {
        switch focusSetting {
        case .auto:
            camera.setContinuousAutoFocus(
                resetExposure: exposureSetting == .auto)
        case .locked:
            camera.lockFocus(lensPosition: lensPosition)
        }
    }

    private func resetCameraControls() {
        zoom = 1
        exposureBias = 0
        focusSetting = .auto
        flashlightOn = false
        whiteBalanceSetting = .auto
        exposureSetting = .auto
        // Manual values reset to mid-range defaults for the new device
        // (the sliders clamp to the active format's real limits).
        whiteBalanceTemperature = 5000
        iso = 200
        shutterSeconds = 1.0 / Double(max(fps, 30))
    }

    // State
    @Published private(set) var status: Status = .idle
    @Published private(set) var isStreaming = false
    @Published var cameraPermissionDenied = false

    /// Per-second stream health for the Live screen's optional overlay.
    /// Sampled by the adaptive-bitrate loop (already 1 Hz), so the overlay
    /// costs nothing beyond what's measured anyway.
    struct StreamHealth: Equatable {
        var fps = 0
        var megabitsPerSecond = 0.0
        var droppedFrames = 0
    }

    @Published private(set) var health: StreamHealth?
    /// Counters are cumulative across connections; baseline them at each
    /// stream start so the overlay shows this stream's drops, not the
    /// app-lifetime total.
    private var healthDroppedBaseline = 0

    /// Keeps resolution/fps within what the selected lens supports.
    func clampCaptureSettings() {
        let supported = { (r: CameraManager.Resolution, f: Int) in
            CameraManager.supports(resolution: r, fps: Int32(f),
                                   lens: self.selectedLens)
        }
        if supported(resolution, fps) { return }
        if supported(resolution, 30) {
            fps = 30
            return
        }
        for fallback in [CameraManager.Resolution.hd1080, .hd720] {
            if supported(fallback, fps) {
                resolution = fallback
                return
            }
            if supported(fallback, 30) {
                resolution = fallback
                fps = 30
                return
            }
        }
    }

    let camera = CameraManager()
    private var encoder: VideoEncoder?
    private let client = StreamClient()
    private var audioReference: AudioReference?

    init() {
        let defaults = UserDefaults.standard
        resolution = CameraManager.Resolution(
            rawValue: defaults.string(forKey: "resolution") ?? "") ?? .hd720
        let storedFps = defaults.integer(forKey: "fps")
        fps = storedFps > 0 ? storedFps : 30
        let lenses = CameraManager.availableLenses()
        availableLenses = lenses.isEmpty ? [CameraManager.defaultLens] : lenses
        let savedLensID = defaults.string(forKey: "selectedLens")
        selectedLens = availableLenses.first { $0.id == savedLensID }
            ?? availableLenses[0]
        codec = VideoCodec(rawValue: defaults.string(forKey: "videoCodec") ?? "")
            ?? (VideoEncoder.isSupported(.hevc) ? .hevc : .h264)
        dimWhileStreaming = defaults.object(forKey: "dimWhileStreaming") as? Bool ?? true
        sendAudioReference = defaults.bool(forKey: "sendAudioReference")
        // didSet doesn't run during init; enforce the exclusivity here.
        sendMicAudio = defaults.bool(forKey: "sendMicAudio")
            && !defaults.bool(forKey: "sendAudioReference")
        selectedMicID = defaults.string(forKey: "selectedMic") ?? "auto"
        remoteStartEnabled = defaults.object(forKey: "remoteStartEnabled") as? Bool ?? true

        client.onStateChange = { [weak self] state in
            Task { @MainActor [weak self] in
                self?.handleClientState(state)
            }
        }
        client.onDiscoveryChange = { [weak self] discoverable in
            Task { @MainActor [weak self] in
                self?.discoverable = discoverable
            }
        }
        client.onControl = { [weak self] json in
            Task { @MainActor [weak self] in
                self?.handleRemoteControl(json)
            }
        }
        client.onFrameDropped = { [weak self] in
            Task { @MainActor [weak self] in
                self?.encoder?.requestKeyframe()
            }
        }
        camera.onInterruption = { [weak self] interrupted in
            Task { @MainActor [weak self] in
                guard let self, self.isStreaming else { return }
                if interrupted {
                    self.status = .error(
                        "Camera paused — in use by another app")
                } else {
                    // Capture restarted; OBS needs a fresh keyframe to
                    // pick the stream back up.
                    self.status = self.lastClientState == .connected
                        ? .streaming : .connecting
                    self.encoder?.requestKeyframe()
                }
            }
        }

        // The broadcast extension listens on the same port; a screen
        // broadcast and the standby listener can't coexist. Release the
        // port whenever the screen is being captured.
        screenCaptured = UIScreen.main.isCaptured
        NotificationCenter.default.addObserver(
            forName: UIScreen.capturedDidChangeNotification,
            object: nil, queue: .main) { [weak self] _ in
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.screenCaptured = UIScreen.main.isCaptured
                self.updateStandby()
            }
        }
    }

    // MARK: - Standby (remote start)
    //
    // While the app is foreground and idle, the listener stays up in
    // "standby": the HELLO advertises `standby: true` and the plugin can
    // send `start_stream` — from its auto-start, the source's "Start
    // camera on the phone" button, or the web panel. The camera never
    // runs in standby; iOS suspends the listener with the app, so this
    // only works while LensLink is on screen (open it by hand, by Siri,
    // or via lenslink://start).

    private var standbyActive = false
    /// Scene foreground state, driven by LensLinkApp.
    private var isForeground = false
    /// A screen broadcast owns port 9979; standby must release it.
    private var screenCaptured = false

    func sceneDidActivate() {
        isForeground = true
        updateStandby()
    }

    func sceneDidEnterBackground() {
        // The camera can't capture in the background; stop cleanly so OBS
        // shows a blank source instead of a frozen frame. The standby
        // listener goes too — iOS would suspend it anyway.
        isForeground = false
        stop()
        stopStandby()
    }

    private func updateStandby() {
        guard !isStreaming else { return }
        let want = remoteStartEnabled && isForeground && !screenCaptured
        if want && !standbyActive {
            standbyActive = true
            client.setStandby(true)
            client.start(port: OBSCProtocol.usbPort)
        } else if !want {
            stopStandby()
        }
    }

    private func stopStandby() {
        guard standbyActive else { return }
        standbyActive = false
        client.setStandby(false)
        client.disconnect()
        if status == .standby {
            status = .idle
        }
    }

    // MARK: - Remote control (from the OBS plugin / web panel)

    private func handleRemoteControl(_ json: Data) {
        guard let object = try? JSONSerialization.jsonObject(with: json),
              let command = object as? [String: Any],
              let cmd = command["cmd"] as? String else { return }

        // Stream lifecycle commands work regardless of streaming state,
        // but only when the user has remote start enabled.
        switch cmd {
        case "start_stream":
            if remoteStartEnabled, !isStreaming {
                Task { await start() }
            }
            return
        case "stop_stream":
            // Paired with the plugin's "Disconnect when hidden": hiding
            // the source stops the camera; showing it starts it again.
            if remoteStartEnabled, isStreaming {
                stop()
            }
            return
        default:
            break
        }

        guard isStreaming else { return }

        // Clamp remote values before storing: the camera clamps for
        // itself, but an out-of-range @Published value would put sliders
        // out of range and misreport state to the web panel.
        switch cmd {
        case "zoom":
            if let value = command["value"] as? Double {
                zoom = min(max(CGFloat(value), 1), camera.maxZoomFactor)
            }
        case "exposure_bias":
            if let value = command["value"] as? Double {
                let range = camera.exposureBiasRange
                exposureBias = min(max(Float(value), range.lowerBound),
                                   range.upperBound)
            }
        case "focus":
            if let position = command["lensPosition"] as? Double {
                lensPosition = min(max(Float(position), 0), 1)
            }
            focusSetting = (command["mode"] as? String) == "locked" ? .locked : .auto
        case "flashlight":
            if let on = command["on"] as? Bool {
                flashlightOn = on
            }
        case "flip":
            flipCamera()
        case "white_balance":
            if let value = (command["temperature"] as? NSNumber)?.floatValue {
                whiteBalanceTemperature = min(max(value, 2500), 8000)
            }
            whiteBalanceSetting =
                (command["mode"] as? String) == "locked" ? .locked : .auto
        case "exposure":
            if let value = (command["iso"] as? NSNumber)?.floatValue {
                let range = camera.isoRange
                iso = min(max(value, range.lowerBound), range.upperBound)
            }
            if let value = (command["shutterSeconds"] as? NSNumber)?.doubleValue {
                shutterSeconds = min(max(value, camera.minShutterSeconds),
                                     camera.maxShutterSeconds(fps: Int32(fps)))
            }
            exposureSetting =
                (command["mode"] as? String) == "manual" ? .manual : .auto
        case "set_format":
            applyRemoteFormat(command)
        case "mic":
            // Validate against the live list: a stale remote UI can name a
            // mic that just left (Bluetooth), and the id must not stick.
            if let id = command["id"] as? String,
               micOptions.contains(where: { $0.id == id }) {
                selectedMicID = id
            }
        case "selectLens":
            if let label = command["label"] as? String,
               let lens = availableLenses.first(where: { $0.label == label }),
               CameraManager.supports(resolution: resolution,
                                      fps: Int32(fps), lens: lens) {
                selectedLens = lens
            }
        default:
            break
        }
    }

    /// Remote format change (resolution / frame rate / codec) from the
    /// plugin's web panel. Any subset of fields may be present; the combo
    /// is validated against the current lens and ignored if unsupported —
    /// the STATE snapshot advertises what's valid, so a remote UI only
    /// offers combos that work.
    private func applyRemoteFormat(_ command: [String: Any]) {
        var newResolution = resolution
        var newFps = fps
        var newCodec = codec

        if let raw = command["resolution"] as? String {
            guard let parsed = CameraManager.Resolution(rawValue: raw) else { return }
            newResolution = parsed
        }
        if let value = (command["fps"] as? NSNumber)?.intValue {
            newFps = value
        }
        if let raw = command["codec"] as? String {
            guard let parsed = VideoCodec(rawValue: raw),
                  VideoEncoder.isSupported(parsed) else { return }
            newCodec = parsed
        }
        guard CameraManager.supports(resolution: newResolution,
                                     fps: Int32(newFps),
                                     lens: selectedLens) else { return }

        let formatChanged = newResolution != resolution || newFps != fps
        let codecChanged = newCodec != codec
        guard formatChanged || codecChanged else { return }
        resolution = newResolution
        fps = newFps
        codec = newCodec
        // reconfigure also rebuilds the encoder on a codec-only change
        // (it compares the running encoder's codec itself).
        reconfigureLiveCapture(formatChanged: formatChanged)
    }

    /// Switches front/back if a lens on the other side supports the
    /// current resolution/fps (the selectedLens observer reconfigures
    /// capture mid-stream).
    func flipCamera() {
        let newPosition: AVCaptureDevice.Position =
            selectedLens.position == .front ? .back : .front
        guard let lens = availableLenses.first(where: {
            $0.position == newPosition &&
            CameraManager.supports(resolution: resolution, fps: Int32(fps),
                                   lens: $0)
        }) else { return }
        selectedLens = lens
    }

    // MARK: - Streaming lifecycle

    /// Set synchronously before the first await: `guard !isStreaming`
    /// alone lets a second tap re-enter during the permission prompt and
    /// leak a live encoder.
    private var isStarting = false

    func start() async {
        guard !isStreaming, !isStarting else { return }
        isStarting = true
        defer { isStarting = false }

        guard await CameraManager.requestPermission() else {
            cameraPermissionDenied = true
            status = .error("Camera access denied — enable it in Settings")
            return
        }

        // Fall back to H.264 automatically if this device can't encode HEVC.
        let activeCodec = (codec == .hevc && !VideoEncoder.isSupported(.hevc))
            ? VideoCodec.h264 : codec

        let size = resolution.size
        let encoder = VideoEncoder(codec: activeCodec,
                                   width: size.width, height: size.height,
                                   fps: Int32(fps),
                                   bitrate: resolution.bitrate(for: activeCodec))
        do {
            try camera.configure(lens: selectedLens,
                                 resolution: resolution,
                                 fps: Int32(fps))
            try encoder.start()
        } catch {
            status = .error(error.localizedDescription)
            return
        }

        encoder.onEncodedFrame = { [weak self] frame in
            self?.client.sendVideoFrame(frame)
        }
        camera.onSampleBuffer = { [weak encoder] sampleBuffer in
            encoder?.encode(sampleBuffer)
        }
        self.encoder = encoder

        isStreaming = true
        status = .connecting
        UIApplication.shared.isIdleTimerDisabled = true

        camera.start()
        resetCameraControls()
        healthDroppedBaseline = client.statsSnapshot().framesDropped
        startAdaptiveBitrate(target: resolution.bitrate(for: activeCodec))
        client.setStandby(false)
        if standbyActive {
            // Remote start: reuse the standby transport. If OBS is already
            // connected it's waiting for the video config — run the
            // on-connect sends now (no state change will fire).
            standbyActive = false
            if lastClientState == .connected {
                handleClientState(.connected)
            }
        } else {
            client.start(port: OBSCProtocol.usbPort)
        }

        if sendAudioReference {
            await startMicCapture(purpose: .lipSyncReference)
        } else if sendMicAudio {
            await startMicCapture(purpose: .playback)
        }
    }

    /// Captures the phone mic — as the lip-sync calibration reference
    /// (packet type 9, never heard) or as the source's playable audio
    /// (packet type 10, same wire format as screen-mirror audio).
    private func startMicCapture(purpose: AudioReference.Purpose) async {
        guard await AudioReference.requestPermission() else {
            micPermissionDenied = true
            return
        }
        let capture = AudioReference(purpose: purpose)
        capture.preferredMicID = selectedMicID
        capture.onPCM = { [weak self] pcm, pts in
            switch purpose {
            case .lipSyncReference:
                self?.client.sendAudio(pcm, ptsNanoseconds: pts)
            case .playback:
                self?.client.sendScreenAudio(pcm, ptsNanoseconds: pts)
            }
        }
        do {
            try capture.start()
            audioReference = capture
            // The STATE snapshot gates its mic fields on capture being
            // up; resend now that it is.
            scheduleStateSend()
        } catch {
            print("Mic capture failed: \(error.localizedDescription)")
        }
    }

    /// Adaptive bitrate: back off quickly when the link drops frames,
    /// recover slowly (+10%/10s of clean streaming) up to the target.
    private var adaptiveTask: Task<Void, Never>?

    private func startAdaptiveBitrate(target: Int) {
        adaptiveTask?.cancel()
        adaptiveTask = Task { [weak self] in
            var current = target
            var stableSeconds = 0
            var previousStats: StreamClient.Stats?
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                guard let self, self.isStreaming else { break }

                // Health overlay sample: per-second deltas of the send
                // counters (the sleep above sets the 1 s window).
                let stats = self.client.statsSnapshot()
                if let previous = previousStats {
                    self.health = StreamHealth(
                        fps: max(0, stats.framesSent - previous.framesSent),
                        megabitsPerSecond: Double(max(0, stats.bytesSent - previous.bytesSent)) * 8 / 1_000_000,
                        droppedFrames: max(0, stats.framesDropped - self.healthDroppedBaseline))
                }
                previousStats = stats

                let dropped = self.client.takeDroppedFrameCount()
                let sendDelayMs = self.client.takeMaxSendDelayMs()
                if dropped > 0 || sendDelayMs > 200 {
                    stableSeconds = 0
                    let reduced = max(1_000_000, current * 3 / 4)
                    if reduced < current {
                        current = reduced
                        self.encoder?.setBitrate(current)
                        print("Adaptive bitrate: congestion (dropped "
                              + "\(dropped), send delay \(sendDelayMs) ms) "
                              + "-> \(current / 1000) kbps")
                    }
                } else if current < target {
                    stableSeconds += 1
                    if stableSeconds >= 10 {
                        stableSeconds = 0
                        current = min(target, current * 11 / 10)
                        self.encoder?.setBitrate(current)
                    }
                }
            }
        }
    }

    func stop() {
        guard isStreaming else { return }
        isStreaming = false
        status = .idle
        UIApplication.shared.isIdleTimerDisabled = false

        adaptiveTask?.cancel()
        adaptiveTask = nil
        health = nil
        if flashlightOn {
            flashlightOn = false
        }
        client.disconnect()
        camera.stop()
        camera.onSampleBuffer = nil
        encoder?.stop()
        encoder = nil
        audioReference?.stop()
        audioReference = nil

        // Back to standby (if enabled and foreground) so OBS can start the
        // camera again without touching the phone. The plugin only
        // auto-starts when the app was previously unreachable, so a manual
        // stop here doesn't bounce straight back into streaming.
        updateStandby()
    }

    /// Last state reported by the client (client.state itself is confined
    /// to the network queue).
    private var lastClientState: StreamClient.State = .disconnected

    private func handleClientState(_ state: StreamClient.State) {
        lastClientState = state
        guard isStreaming else {
            handleStandbyClientState(state)
            return
        }
        switch state {
        case .connected:
            status = .streaming
            let size = resolution.size
            client.sendVideoConfig(codec: encoder?.codec ?? codec,
                                   width: size.width, height: size.height,
                                   fps: Int32(fps))
            // Fresh connection: make sure OBS gets a decodable frame ASAP,
            // and seed the remote UI with the current control state.
            encoder?.requestKeyframe()
            client.sendState(controlStateSnapshot())
        case .failed(let message):
            // The client keeps its listener alive across connection drops
            // and reports .connecting; .failed means the listener itself
            // died — fatal.
            status = .error(message)
            stopKeepingError()
        case .disconnected, .connecting:
            status = .connecting
        }
    }

    /// Status while idle in standby. Only the idle↔standby words are
    /// managed here — an error message stays visible (the standby listener
    /// still works underneath it).
    private func handleStandbyClientState(_ state: StreamClient.State) {
        guard standbyActive else { return }
        switch state {
        case .connected:
            if status == .idle || status == .standby {
                status = .standby
            }
        case .disconnected, .connecting:
            if status == .standby {
                status = .idle
            }
        case .failed(let message):
            // The listener itself died (e.g. the port is taken); standby
            // can't work until something changes.
            standbyActive = false
            client.disconnect()
            status = .error(message)
        }
    }

    private func stopKeepingError() {
        let currentStatus = status
        stop()
        status = currentStatus
    }
}
