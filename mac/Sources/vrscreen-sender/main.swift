import AppKit
import CoreVideo
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

let queue = DispatchQueue(label: "vrscreen.sender", qos: .userInteractive)
let encoder = VideoEncoder(codec: options.codec, bitrate: Int(options.bitrateMbps * 1_000_000),
                           fps: options.fps, keyframeIntervalSeconds: options.keyframeIntervalSeconds)
let server = MirrorServer(port: options.port, maxFramesInFlight: UInt32(options.maxFramesInFlight), queue: queue)
let pipeline = SenderPipeline(queue: queue, encoder: encoder, server: server)
let discovery = options.discoveryPort.map { DiscoveryResponder(port: $0, tcpPort: options.port, queue: queue) }
server.controlOnConnect = options.controlOnConnect
// Curvature/dim persist across restarts and are sent to every client, with or without the menu.
let settingsStore = options.settingsPath.map { DisplaySettingsStore(url: URL(fileURLWithPath: $0)) }
    ?? DisplaySettingsStore.standard
let initialDisplaySettings = settingsStore.load()
server.updateDisplaySettings(initialDisplaySettings)
let deliver: (CVPixelBuffer, UInt64) -> Void = { buffer, micros in pipeline.handleCapture(buffer, captureMicros: micros) }
// Audio: nil with --no-audio. The menu's "Audio in headset" switches capture on and off.
let audioPipeline = options.audio ? AudioPipeline(server: server, serverQueue: queue) : nil
let macMute = MacOutputMute(queue: queue)
/// Latest settings (from the file, then the menu). On `queue`.
var currentSettings = initialDisplaySettings
/// Menu status-line updates; set once the menu exists. On `queue`.
var onClientChangedForMenu: ((String?) -> Void)?

var testSource: TestPatternSource?
var screenSource: ScreenSource?

var audioWanted: Bool { audioPipeline != nil && currentSettings.audio }

/// Applies the audio settings: capture on/off, AUDIO_CONFIG for clients, and the Mac mute,
/// which is engaged only while audio actually goes to a connected client. On `queue`.
func applyAudioSettings() {
    let on = audioWanted
    if on != (server.audioConfig != nil) {
        server.audioConfig = on ? AudioPipeline.config : nil
        if !on { audioPipeline?.reset() }
        log("audio: \(on ? "on (48 kHz stereo s16le, 10 ms chunks to the headset)" : "off")")
    }
    testSource?.setAudio(on)
    screenSource?.setAudioCapture(on)
    macMute.set(on && currentSettings.muteMac && server.activeClient != nil)
}

server.onActiveClientChanged = { address in
    applyAudioSettings()
    onClientChangedForMenu?(address)
}

queue.async {
    do {
        try server.start()
        try discovery?.start()
        switch options.source {
        case .test:
            let source = TestPatternSource(
                width: options.width, height: options.height, fps: options.fps, queue: queue,
                audioHandler: audioPipeline.map { audio in
                    { samples, micros in audio.queue.async { audio.handle(interleaved: samples, captureMicros: micros) } }
                },
                handler: deliver)
            try source.start()
            testSource = source
        case .screen:
            let source = ScreenSource(displayIndex: options.displayIndex, fps: options.fps,
                                      maxWidth: options.maxWidth, queue: queue, audio: audioPipeline,
                                      capturesAudio: audioWanted, handler: deliver)
            screenSource = source
            source.start { error in
                guard let error else { return }
                log("source: screen capture failed to start: \(error)")
                log("source: grant Screen Recording to the app running this binary (System Settings → Privacy & Security → Screen & System Audio Recording), restart it, or use --source test")
                exit(1)
            }
        }
        applyAudioSettings()
        if audioPipeline == nil { log("audio: off (--no-audio)") }
    } catch {
        log("startup failed: \(error)")
        exit(1)
    }
}

let statsTimer = DispatchSource.makeTimerSource(queue: queue)
statsTimer.schedule(deadline: .now() + 1, repeating: 1)
statsTimer.setEventHandler {
    // The audio queue only converts and never waits on `queue`, so this sync is short.
    pipeline.logStats(audioCapture: audioPipeline.flatMap { audio in audio.queue.sync { audio.takeCaptureStats() } })
    screenSource?.checkDisplayMode()
}
statsTimer.resume()

/// Clean shutdown on `queue`: Ctrl-C, SIGTERM, SIGHUP (terminal closed) and the menu's
/// Quit all come here. Every path ends in exit(), whose atexit handler also restores the
/// cursor if a mouse gesture had frozen it.
func shutdown() {
    log("shutting down")
    CursorFreeze.release()
    macMute.set(false)
    statsTimer.cancel()
    testSource?.stop()
    server.stop()
    discovery?.stop()
    pipeline.stop()
    if let screen = screenSource {
        screen.stop { exit(0) }
        // Do not hang on a stuck capture stop.
        queue.asyncAfter(deadline: .now() + 2) { exit(0) }
    } else {
        exit(0)
    }
}

signal(SIGINT, SIG_IGN)
signal(SIGTERM, SIG_IGN)
signal(SIGHUP, SIG_IGN)
var signalSources: [DispatchSourceSignal] = []
for sig in [SIGINT, SIGTERM, SIGHUP] {
    let source = DispatchSource.makeSignalSource(signal: sig, queue: queue)
    source.setEventHandler { shutdown() }
    source.resume()
    signalSources.append(source)
}

// Shortcuts, mouse gestures and the menu-bar icon need the main thread to run an
// AppKit/Carbon event loop; the accessory policy keeps the sender out of the Dock and
// the app switcher. Signals are still handled on `queue`, so Ctrl-C shuts down as before.
var hotkeys: HotkeyController?
var mouse: MouseControl?
var statusMenu: StatusMenu?
var lastMouseDropLog = Date.distantPast
if options.hotkeys || options.mouse || options.menu {
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    if options.menu {
        let travelModeSource: StatusMenu.TravelModeSource
        if let serial = options.adbSerial {
            if let executable = HeadsetAdb.locateExecutable() {
                travelModeSource = .adb(HeadsetAdb(executable: executable, serial: serial))
            } else {
                travelModeSource = .unavailable(.adbMissing)
            }
        } else {
            travelModeSource = .unavailable(.notOnUSB)
        }
        let menu = StatusMenu(
            store: settingsStore, settings: initialDisplaySettings, travelModeSource: travelModeSource,
            audioAvailable: audioPipeline != nil,
            onSettingsChange: { settings in
                queue.async {
                    server.updateDisplaySettings(settings)
                    currentSettings = settings
                    applyAudioSettings()
                }
            },
            onCommand: { command in
                queue.async {
                    if let address = server.sendControl(command) {
                        log("menu: \(command) → \(address)")
                    } else {
                        log("menu: \(command): no client connected, dropped")
                    }
                }
            },
            onQuit: { queue.async { shutdown() } })
        menu.install()
        queue.async {
            // Server state lives on `queue`; also show a client that connected before this ran.
            onClientChangedForMenu = { address in DispatchQueue.main.async { menu.setClient(address: address) } }
            let current = server.activeClient?.address
            DispatchQueue.main.async { menu.setClient(address: current) }
        }
        statusMenu = menu
    }
    if options.hotkeys {
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
    }
    if options.mouse {
        // MouseControl logs a per-second summary of what it sends; drops are logged at most once a second.
        let control = MouseControl(degreesPerPoint: options.mouseDegreesPerPoint,
                                   scrollSensitivity: options.scrollSensitivity) { commands in
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
    }
    app.run()
} else {
    dispatchMain()
}
