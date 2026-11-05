import AppKit
import CoreGraphics
import Foundation
import MirrorProtocol

setvbuf(stdout, nil, _IOLBF, 0)

let options: SenderOptions
do {
    options = try SenderOptions.parse(Array(CommandLine.arguments.dropFirst()))
} catch {
    FileHandle.standardError.write("error: \(error)\n\n\(SenderOptions.usage)\n".data(using: .utf8)!)
    exit(2)
}

do {
    try LogFile.shared.open(LogFile.standardURL)
} catch {
    print("warning: cannot write \(LogFile.standardURL.path): \(error)")
}
log("VRScreen \(AppInfo.version) (pid \(getpid())"
    + (AppInfo.isAppBundle ? ", \(Bundle.main.bundlePath))" : ", \(CommandLine.arguments[0]))")
    + (CommandLine.arguments.count > 1 ? " " + CommandLine.arguments.dropFirst().joined(separator: " ") : ""))

// One VRScreen.app at a time (a bare dev binary is not checked: the port check covers it).
if AppInfo.isAppBundle,
   let other = NSRunningApplication.runningApplications(withBundleIdentifier: AppInfo.bundleIdentifier)
       .first(where: { $0.processIdentifier != getpid() }) {
    log("VRScreen is already running (pid \(other.processIdentifier)); exiting")
    exit(0)
}

// MARK: Settings (main thread)

let settingsModel = SettingsModel(
    store: options.settingsPath.map { SettingsStore(url: URL(fileURLWithPath: $0)) } ?? .standard,
    overrides: options.overrides)
/// The effective settings last applied. Main thread.
var applied = settingsModel.effective
/// --display N, until a display is chosen in the menu. Main thread.
var displayIndexOverride = options.displayIndex
log("settings: \(applied.summary) (\(settingsModel.store.url.path))")

func videoSettings(_ s: SenderSettings) -> VideoSettings {
    VideoSettings(maxWidth: s.maxWidth, fps: s.fps, codec: s.codec.videoCodec, bitrateMbps: s.bitrateMbps,
                  display: displayIndexOverride.map { .index($0) } ?? s.displayID.map { .id($0) } ?? .main)
}

// MARK: Streaming (on `queue`)

let queue = DispatchQueue(label: "vrscreen.sender", qos: .userInteractive)
let server = MirrorServer(port: options.port, maxFramesInFlight: UInt32(options.maxFramesInFlight), queue: queue)
let pipeline = SenderPipeline(
    queue: queue,
    encoder: CaptureController.makeEncoder(videoSettings(applied), keyframeIntervalSeconds: options.keyframeIntervalSeconds),
    server: server)
let discovery = options.discoveryPort.map { DiscoveryResponder(port: $0, tcpPort: options.port, queue: queue) }
server.controlOnConnect = options.controlOnConnect
// Curvature/dim/hands are sent to every client, with or without the menu.
server.updateHeadsetSettings(applied)
// Audio: nil with --no-audio. The menu's "Audio in headset" switches capture on and off.
let audioPipeline = options.audio ? AudioPipeline(server: server, serverQueue: queue) : nil
let macMute = MacOutputMute(queue: queue)
/// Latest settings and video settings, on `queue`.
var queueSettings = applied
var queueVideo = videoSettings(applied)
/// Created once the TCP port is ours. On `queue`.
var capture: CaptureController?
var captureEverRan = false
/// The Core Audio process tap, when it is the audio source (screen mode). On `queue`.
var processTap: SystemAudioTap?
/// Last HEADSET_STATUS of the current connection (changes are logged once). On `queue`.
var lastHeadsetStatus: HeadsetStatus?
var reportedPortInUse: UInt16?

var audioWanted: Bool { audioPipeline != nil && queueSettings.audio }

/// Applies the audio settings: capture on/off, AUDIO_CONFIG for clients, and the Mac mute,
/// which is engaged only while audio actually goes to a connected client. On `queue`.
func applyAudioSettings() {
    let on = audioWanted
    if on != (server.audioConfig != nil) {
        server.audioConfig = on ? AudioPipeline.config : nil
        if !on { audioPipeline?.reset() }
        log("audio: \(on ? "on (48 kHz stereo s16le, 10 ms chunks to the headset)" : "off")")
    }
    capture?.setAudio(on)
    processTap?.setRunning(on)
    let mute = on && queueSettings.muteMac && server.activeClient != nil
    if let processTap {
        processTap.setMuted(mute) // the tap's own mute: nothing to restore
    } else {
        macMute.set(mute)
    }
}

/// Screen mode: the process tap unless --audio-source sck or it is unavailable (logged and
/// shown as a problem); nil means ScreenCaptureKit audio. Exits for --audio-source tap
/// without a tap. On `queue`.
func makeProcessTap(_ audio: AudioPipeline) -> SystemAudioTap? {
    let reason: String
    switch options.audioSource {
    case .sck:
        log("audio: using ScreenCaptureKit (--audio-source sck)")
        return nil
    case .auto, .tap:
        if #available(macOS 14.2, *) {
            do {
                return try ProcessTapCapture(pipeline: audio)
            } catch {
                reason = "\(error)"
            }
        } else {
            reason = "needs macOS 14.2"
        }
    }
    if options.audioSource == .tap {
        log("audio: process tap unavailable (\(reason)) and --audio-source tap was given")
        exit(1)
    }
    log("audio: process tap unavailable (\(reason)), using ScreenCaptureKit")
    DispatchQueue.main.async { updateHealth { $0.tapUnavailableReason = reason } }
    return nil
}

/// Binds the TCP port (and the discovery port), then starts capture and USB. A port held by
/// another process (another VRScreen or a dev sender) is shown in the menu and retried every
/// 3 s; without the menu it is fatal. On `queue`.
func startNetwork() {
    do {
        if !server.isListening {
            try server.start()
            startCapture()
            DispatchQueue.main.async { usbLink?.start() }
        }
        if let discovery, !discovery.isRunning { try discovery.start() }
        if reportedPortInUse != nil {
            reportedPortInUse = nil
            DispatchQueue.main.async { updateHealth { $0.portInUse = nil } }
        }
    } catch let error as SocketError where error.addressInUse && options.menu {
        if reportedPortInUse != error.port {
            reportedPortInUse = error.port
            log("\(error): another VRScreen or vrscreen-sender? Retrying every 3 s")
            DispatchQueue.main.async { updateHealth { $0.portInUse = error.port } }
        }
        queue.asyncAfter(deadline: .now() + 3) { startNetwork() }
    } catch {
        log("startup failed: \(error)")
        exit(1)
    }
}

func startCapture() {
    guard capture == nil else { return }
    var sourceAudio = audioPipeline
    if options.source == .screen, let audio = audioPipeline {
        audio.trackOutputLatency()
        processTap = makeProcessTap(audio)
        if processTap != nil { sourceAudio = nil }
    }
    let controller = CaptureController(
        source: options.source, testSize: (options.width, options.height),
        keyframeIntervalSeconds: options.keyframeIntervalSeconds, video: queueVideo, audioOn: audioWanted,
        sourceAudio: sourceAudio, queue: queue, pipeline: pipeline)
    controller.onStatus = { status in
        switch status {
        case .starting:
            break
        case .running(let displayID):
            captureEverRan = true
            DispatchQueue.main.async {
                statusMenu?.setCapturedDisplay(displayID)
                updateHealth {
                    $0.captureError = nil
                    $0.captureErrorIsPermission = false
                }
            }
        case .failed(let error, let isPermission):
            if !options.menu && !captureEverRan {
                log("source: grant Screen Recording to the app running this binary (System Settings → Privacy & Security → Screen & System Audio Recording), restart it, or use --source test")
                exit(1)
            }
            DispatchQueue.main.async {
                updateHealth {
                    $0.captureError = error
                    $0.captureErrorIsPermission = isPermission
                }
            }
        }
    }
    capture = controller
    controller.start()
    applyAudioSettings()
    if audioPipeline == nil { log("audio: off (--no-audio)") }
}

server.onActiveClientChanged = { address in
    lastHeadsetStatus = nil
    applyAudioSettings()
    DispatchQueue.main.async {
        statusMenu?.setClient(address: address)
        updateHealth {
            $0.battery = nil
            // A Wi-Fi client got through: the LAN works.
            if let address, !address.hasPrefix("127.") { $0.localNetworkDenied = false }
        }
    }
}
server.onHeadsetStatus = { status in
    guard status != lastHeadsetStatus else { return }
    if lastHeadsetStatus.map(StatusMenu.batteryLogText) != StatusMenu.batteryLogText(status) {
        log(StatusMenu.batteryLogText(status))
    }
    lastHeadsetStatus = status
    DispatchQueue.main.async {
        statusMenu?.setBattery(status)
        updateHealth { $0.battery = status }
    }
}
server.onLANUnreachable = { address in
    log("server: \(address) unreachable (no route to host): Local Network access denied?")
    DispatchQueue.main.async { updateHealth { $0.localNetworkDenied = true } }
}
discovery?.onAnnounceResult = { code in
    let denied = code == EHOSTUNREACH || code == ENETUNREACH
    if code == nil || denied {
        DispatchQueue.main.async { updateHealth { $0.localNetworkDenied = denied } }
    }
}
pipeline.onEncoderError = { error in
    DispatchQueue.main.async { updateHealth { $0.encoderError = error } }
}

let statsTimer = DispatchSource.makeTimerSource(queue: queue)
statsTimer.schedule(deadline: .now() + 1, repeating: 1)
statsTimer.setEventHandler {
    guard let capture else { return } // waiting for the port
    // The audio queue only converts and never waits on `queue`, so this sync is short.
    pipeline.logStats(audioCapture: audioPipeline.flatMap { audio in audio.queue.sync { audio.takeCaptureStats() } })
    capture.checkDisplayMode()
}
statsTimer.resume()

// MARK: Problems (main thread)

var health = HealthInputs(capturesScreen: options.source == .screen, audioWanted: audioPipeline != nil && applied.audio,
                          usb: options.adb ? UsbStatus(link: .noHeadset) : nil, canRelaunch: AppInfo.isAppBundle)
var shownProblems: [Problem] = []

func updateHealth(_ change: (inout HealthInputs) -> Void) {
    change(&health)
    let problems = ProblemList.build(health)
    guard problems != shownProblems else { return }
    shownProblems = problems
    log("problems: " + (problems.isEmpty ? "none" : problems.map(\.title).joined(separator: "; ")))
    statusMenu?.setProblems(problems)
}

/// The cheap permission checks, at startup and whenever the menu opens.
func runChecks() {
    updateHealth {
        $0.screenRecordingGranted = options.source == .screen ? CGPreflightScreenCaptureAccess() : true
        $0.systemAudioPermission = SystemAudioPermission.preflight()
    }
}

func perform(_ action: Problem.Action) {
    let privacy = "x-apple.systempreferences:com.apple.preference.security?"
    log("menu: \(action.title)")
    switch action {
    case .openScreenRecordingSettings, .openSystemAudioSettings:
        NSWorkspace.shared.open(URL(string: privacy + "Privacy_ScreenCapture")!)
    case .openLocalNetworkSettings:
        NSWorkspace.shared.open(URL(string: privacy + "Privacy_LocalNetwork")!)
    case .relaunch: relaunch()
    case .showLogs: showLogs()
    case .copyAdbInstallCommand: StatusMenu.copyAdbInstallCommand()
    case .openPlatformToolsPage: NSWorkspace.shared.open(Adb.downloadPage)
    case .restartAdb: usbLink?.restartAdb()
    case .installHeadsetApp: usbLink?.installApp()
    }
}

/// Opens the log file in Console (or shows it in Finder).
func showLogs() {
    let url = LogFile.shared.url ?? LogFile.standardURL
    if let console = NSWorkspace.shared.urlForApplication(withBundleIdentifier: "com.apple.Console") {
        NSWorkspace.shared.open([url], withApplicationAt: console, configuration: NSWorkspace.OpenConfiguration())
    } else {
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }
}

/// VRScreen.app only: a detached shell waits for this process to exit, then opens the app again.
func relaunch() {
    guard AppInfo.isAppBundle else { return }
    let helper = Process()
    helper.executableURL = URL(fileURLWithPath: "/bin/sh")
    helper.arguments = ["-c", "while kill -0 \(getpid()) 2>/dev/null; do sleep 0.2; done; exec /usr/bin/open \"$0\"",
                        Bundle.main.bundlePath]
    do {
        try helper.run()
        log("relaunching VRScreen")
        requestShutdown()
    } catch {
        log("relaunch failed: \(error)")
    }
}

// MARK: USB (main thread)

let usbLink: UsbHeadsetLink? = options.adb
    ? UsbHeadsetLink(port: options.port, preferredSerial: options.adbSerial, bundledAPK: AppInfo.bundledHeadsetAPK,
                     launchOnAttach: applied.launchHeadsetApp) { status in
        statusMenu?.setUsb(status)
        updateHealth { $0.usb = status }
    }
    : nil
if usbLink == nil { log("usb: off (--no-adb); the headset connects over Wi-Fi") }

// MARK: Settings changes (main thread)

settingsModel.onChange = { settings in applySettings(settings) }

func applySettings(_ s: SenderSettings) {
    let old = applied
    applied = s
    let video = videoSettings(s)
    queue.async {
        server.updateHeadsetSettings(s)
        queueSettings = s
        applyAudioSettings()
        queueVideo = video
        capture?.apply(video)
    }
    if s.hotkeys != old.hotkeys { setHotkeys(s.hotkeys) }
    if s.mouseGestures != old.mouseGestures { setMouse(s.mouseGestures) }
    if s.mouseSensitivity != old.mouseSensitivity || s.scrollSensitivity != old.scrollSensitivity {
        mouse?.setSensitivity(degreesPerPoint: s.mouseSensitivity, scrollSensitivity: s.scrollSensitivity)
    }
    if s.launchHeadsetApp != old.launchHeadsetApp { usbLink?.setLaunchOnAttach(s.launchHeadsetApp) }
    if s.audio != old.audio { updateHealth { $0.audioWanted = audioPipeline != nil && s.audio } }
}

// MARK: Shutdown

var shuttingDown = false

/// Ctrl-C, SIGTERM, SIGHUP (terminal closed), Quit, logout and relaunch all come here (main
/// thread): the pending settings are saved, then `shutdown` runs on `queue`.
func requestShutdown() {
    guard !shuttingDown else { return }
    shuttingDown = true
    settingsModel.flushSave()
    queue.async { shutdown() }
}

/// Every path ends in exit(), whose atexit handlers also restore the cursor and remove the
/// adb reverse if that has not happened yet.
func shutdown() {
    log("shutting down")
    CursorFreeze.release()
    macMute.set(false)
    processTap?.invalidate()
    statsTimer.cancel()
    server.stop()
    discovery?.stop()
    pipeline.stop()
    UsbHeadsetLink.removeCreatedReverse()
    guard let capture else { exit(0) }
    capture.stop { exit(0) }
    // Do not hang on a stuck capture stop.
    queue.asyncAfter(deadline: .now() + 2) { exit(0) }
}

var signalSources: [DispatchSourceSignal] = []
for sig in [SIGINT, SIGTERM, SIGHUP, SIGUSR1] {
    signal(sig, SIG_IGN)
    let source = DispatchSource.makeSignalSource(signal: sig, queue: .main)
    source.setEventHandler { sig == SIGUSR1 ? settingsModel.reload() : requestShutdown() }
    source.resume()
    signalSources.append(source)
}

// MARK: AppKit: menu, shortcuts, mouse (main thread)

/// Logout / `quit` Apple Event: the same clean shutdown. Re-opening the app from Finder
/// while it runs opens its menu.
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        requestShutdown()
        return .terminateLater
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        DispatchQueue.main.async { statusMenu?.open() }
        return false
    }
}

var hotkeys: HotkeyController?
var mouse: MouseControl?
var statusMenu: StatusMenu?
var lastMouseDropLog = Date.distantPast
let appDelegate = AppDelegate()

func setHotkeys(_ on: Bool) {
    if on, hotkeys == nil {
        let controller = HotkeyController { binding, isRepeat in
            queue.async {
                let action = "hotkey \(binding.label) → \(binding.command)\(isRepeat ? " (repeat)" : "")"
                if let address = server.sendControl(binding.command) {
                    log("\(action) → \(address)")
                } else {
                    log("\(action): no client connected, dropped")
                }
            }
        }
        controller.register()
        hotkeys = controller
    } else if !on, let controller = hotkeys {
        controller.unregister()
        hotkeys = nil
        log("hotkeys: off")
    }
}

func setMouse(_ on: Bool) {
    if on, mouse == nil {
        // MouseControl logs a per-second summary of what it sends; drops are logged at most once a second.
        let control = MouseControl(degreesPerPoint: applied.mouseSensitivity,
                                   scrollSensitivity: applied.scrollSensitivity) { commands in
            queue.async {
                for command in commands where server.sendControl(command) == nil {
                    if Date().timeIntervalSince(lastMouseDropLog) >= 1 {
                        lastMouseDropLog = Date()
                        log("mouse: \(command): no client connected, dropped")
                    }
                }
            }
        }
        control.start()
        mouse = control
    } else if !on, let control = mouse {
        control.stop()
        mouse = nil
        log("mouse: off")
    }
}

queue.async { startNetwork() }

// Shortcuts, mouse gestures and the menu-bar icon need the main thread to run an
// AppKit/Carbon event loop; the accessory policy keeps the sender out of the Dock and
// the app switcher (VRScreen.app also sets LSUIElement).
if options.menu || applied.hotkeys || applied.mouseGestures {
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    app.delegate = appDelegate
    if options.menu {
        let menu = StatusMenu(
            model: settingsModel, usb: usbLink, audioAvailable: audioPipeline != nil,
            callbacks: StatusMenu.Callbacks(
                onCommand: { command in
                    queue.async {
                        if let address = server.sendControl(command) {
                            log("menu: \(command) → \(address)")
                        } else {
                            log("menu: \(command): no client connected, dropped")
                        }
                    }
                },
                onDisplayChosen: { id in
                    displayIndexOverride = nil
                    settingsModel.set(\.displayID, id)
                    applySettings(settingsModel.effective) // also when only the override went away
                },
                onProblemAction: perform,
                onMenuWillOpen: runChecks,
                onShowLogs: showLogs,
                onQuit: requestShutdown))
        statusMenu = menu
        menu.install()
        menu.setProblems(shownProblems)
        if !options.adb { menu.setUsb(UsbStatus(link: .off)) }
    }
    runChecks()
    setHotkeys(applied.hotkeys)
    setMouse(applied.mouseGestures)
    app.run()
} else {
    runChecks()
    dispatchMain()
}
