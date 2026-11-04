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
let settingsStore = DisplaySettingsStore.standard
let initialDisplaySettings = settingsStore.load()
server.updateDisplaySettings(initialDisplaySettings)
let deliver: (CVPixelBuffer, UInt64) -> Void = { buffer, micros in pipeline.handleCapture(buffer, captureMicros: micros) }

var testSource: TestPatternSource?
var screenSource: ScreenSource?

queue.async {
    do {
        try server.start()
        try discovery?.start()
        switch options.source {
        case .test:
            let source = TestPatternSource(width: options.width, height: options.height, fps: options.fps,
                                           queue: queue, handler: deliver)
            try source.start()
            testSource = source
        case .screen:
            let source = ScreenSource(displayIndex: options.displayIndex, fps: options.fps,
                                      maxWidth: options.maxWidth, queue: queue, handler: deliver)
            screenSource = source
            source.start { error in
                guard let error else { return }
                log("source: screen capture failed to start: \(error)")
                log("source: grant Screen Recording to the app running this binary (System Settings → Privacy & Security → Screen & System Audio Recording), restart it, or use --source test")
                exit(1)
            }
        }
    } catch {
        log("startup failed: \(error)")
        exit(1)
    }
}

let statsTimer = DispatchSource.makeTimerSource(queue: queue)
statsTimer.schedule(deadline: .now() + 1, repeating: 1)
statsTimer.setEventHandler {
    pipeline.logStats()
    screenSource?.checkDisplayMode()
}
statsTimer.resume()

/// Clean shutdown on `queue`: Ctrl-C, SIGTERM, SIGHUP (terminal closed) and the menu's
/// Quit all come here. Every path ends in exit(), whose atexit handler also restores the
/// cursor if a mouse gesture had frozen it.
func shutdown() {
    log("shutting down")
    CursorFreeze.release()
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
            onSettingsChange: { settings in queue.async { server.updateDisplaySettings(settings) } },
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
            server.onActiveClientChanged = { address in DispatchQueue.main.async { menu.setClient(address: address) } }
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
