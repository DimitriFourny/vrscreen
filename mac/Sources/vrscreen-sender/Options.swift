import Foundation
import MirrorProtocol

struct SenderOptions {
    enum Source: String { case screen, test }
    /// Where screen-mode audio comes from: `auto` tries the process tap, then ScreenCaptureKit.
    enum AudioSource: String { case auto, tap, sck }

    var source: Source = .screen
    /// --display N: SCK display index for this session (the menu's Display choice replaces it).
    var displayIndex: Int?
    var port: UInt16 = WireProtocol.defaultPort
    /// UDP port for DISCOVER/ANNOUNCE; nil with --no-discovery.
    var discoveryPort: UInt16? = DiscoveryMessage.defaultPort
    var keyframeIntervalSeconds = 5.0
    var maxFramesInFlight = 3
    var width = 1920
    var height = 1080
    var menu = true
    /// False with --no-audio: never capture or send audio, whatever the menu setting.
    var audio = true
    /// --settings: settings file instead of the standard one.
    var settingsPath: String?
    var audioSource: AudioSource = .auto
    /// False with --no-adb: no adb at all (Wi-Fi only).
    var adb = true
    /// --adb-serial: the headset to use when several adb devices are attached.
    var adbSerial: String?
    /// Settings given on the command line: they apply for this session only, on top of
    /// the stored settings, until the same setting is changed in the menu.
    var overrides = SettingsOverrides()
    /// Steps sent to every client right after it connects (testing).
    var controlOnConnect: [DebugControlStep] = []

    static let usage = """
    Usage: vrscreen-sender [options]

    Everything the menu offers is a saved setting; these options override the saved value
    for this session only (until it is changed in the menu):
      --fps N                  Target frame rate (menu: 60 or 30; default 60).
      --max-width N            Scale the display down to fit this width (menu: 3840,
                               2560 or 1920; default 3840).
      --codec hevc|h264        Video codec (default hevc).
      --bitrate MBPS           Average bitrate in Mbit/s (menu: 15, 30 or 50; default 30).
      --no-hotkeys             Do not register the global ⌃⌥⌘ screen-placement shortcuts.
      --no-mouse               Do not use ⌃⌥⌘ + mouse (orbit) and ⌃⌥⌘ + scroll (zoom).
      --mouse-sensitivity DEG  Orbit degrees per point of mouse movement (default 0.1).
      --scroll-sensitivity X   Scroll zoom multiplier (default 1; a wheel notch is a
                               quarter DISTANCE/SIZE step).
      --no-launch              Do not start the headset app when the headset appears on USB.

    Developer options:
      --source screen|test     Frame source (default screen). "test" is a synthetic
                               pattern that needs no Screen Recording permission; it
                               flashes white once per second, with a 1 kHz beep in
                               the audio at the flash.
      --display N              Display index from the list printed at startup (default:
                               the display chosen in the menu, else the main display).
      --port N                 TCP listen port (default 7777).
      --discovery-port N       UDP port answering LAN discovery (default 7778).
      --no-discovery           Do not answer LAN discovery.
      --keyframe-interval S    Max seconds between keyframes (default 5).
      --max-in-flight N        For clients that send FRAME_ACK: max frames sent but
                               not yet consumed by the client (default 3).
      --width N --height N     Test source size before --max-width (default 1920x1080).
      --no-menu                No menu-bar icon.
      --no-audio               Never capture or send audio (overrides the menu's
                               "Audio in headset").
      --audio-source auto|tap|sck
                               System audio source with --source screen (default auto):
                               the Core Audio process tap (~5 ms, needs System Audio
                               Recording permission, macOS 14.2+), falling back to
                               ScreenCaptureKit audio (15-50 ms later).
      --settings PATH          Settings file to read and write (default
                               ~/Library/Application Support/VRScreen/settings.json),
                               e.g. for a second sender used for testing.
      --no-adb                 Do not use adb (no USB: the headset connects over Wi-Fi).
      --adb-serial S           The headset's adb serial, when several devices are attached.
      --debug-control-on-connect SPEC
                               Testing: send these CONTROL commands to each client on
                               connect, e.g. "translate:1,0;orbit:500,-250;distance:-2;
                               distance_fine:25;size:1;size_fine:-50;recenter;reset".
      --version                Print the version.
      -h, --help               Show this help.

    kill -USR1 <pid> re-reads the settings file (after editing it by hand).
    """

    static func parse(_ args: [String]) throws -> SenderOptions {
        var o = SenderOptions()
        var it = args.makeIterator()
        func value(_ name: String) throws -> String {
            guard let v = it.next() else { throw OptionError("\(name) needs a value") }
            return v
        }
        func int(_ name: String, min: Int = 1) throws -> Int {
            let s = try value(name)
            guard let v = Int(s), v >= min else { throw OptionError("\(name): invalid value '\(s)'") }
            return v
        }
        func double(_ name: String) throws -> Double {
            let s = try value(name)
            guard let v = Double(s), v > 0, v.isFinite else { throw OptionError("\(name): invalid value '\(s)'") }
            return v
        }
        while let arg = it.next() {
            switch arg {
            case "--source":
                let s = try value(arg)
                guard let src = Source(rawValue: s) else { throw OptionError("--source must be screen or test") }
                o.source = src
            case "--display": o.displayIndex = try int(arg, min: 0)
            case "--port":
                let p = try int(arg)
                guard let port = UInt16(exactly: p) else { throw OptionError("--port out of range") }
                o.port = port
            case "--discovery-port":
                let p = try int(arg)
                guard let port = UInt16(exactly: p) else { throw OptionError("--discovery-port out of range") }
                o.discoveryPort = port
            case "--no-discovery": o.discoveryPort = nil
            case "--fps": o.overrides.set(\.fps, min(try int(arg), 240))
            case "--max-width": o.overrides.set(\.maxWidth, min(try int(arg, min: 16), 16_384))
            case "--codec":
                switch try value(arg).lowercased() {
                case "hevc", "h265": o.overrides.set(\.codec, .hevc)
                case "h264", "avc": o.overrides.set(\.codec, .h264)
                default: throw OptionError("--codec must be hevc or h264")
                }
            case "--bitrate": o.overrides.set(\.bitrateMbps, try double(arg))
            case "--keyframe-interval": o.keyframeIntervalSeconds = try double(arg)
            case "--max-in-flight": o.maxFramesInFlight = try int(arg)
            case "--width": o.width = try int(arg, min: 16) & ~1
            case "--height": o.height = try int(arg, min: 16) & ~1
            case "--no-hotkeys": o.overrides.set(\.hotkeys, false)
            case "--no-mouse": o.overrides.set(\.mouseGestures, false)
            case "--no-menu": o.menu = false
            case "--no-audio": o.audio = false
            case "--settings": o.settingsPath = try value(arg)
            case "--audio-source":
                let s = try value(arg)
                guard let source = AudioSource(rawValue: s) else { throw OptionError("--audio-source must be auto, tap or sck") }
                o.audioSource = source
            case "--no-adb": o.adb = false
            case "--adb-serial": o.adbSerial = try value(arg)
            case "--no-launch": o.overrides.set(\.launchHeadsetApp, false)
            case "--mouse-sensitivity": o.overrides.set(\.mouseSensitivity, try double(arg))
            case "--scroll-sensitivity": o.overrides.set(\.scrollSensitivity, try double(arg))
            case "--debug-control-on-connect": o.controlOnConnect = try parseControlSpec(try value(arg))
            case "--version":
                print("VRScreen \(AppInfo.version)")
                exit(0)
            case "-h", "--help":
                print(usage)
                exit(0)
            default:
                throw OptionError("unknown option '\(arg)'")
            }
        }
        return o
    }

    /// Parses `cmd[:args][*N];...` with cmd one of recenter, reset, translate:DX,DY,
    /// distance:STEPS, size:STEPS (-128...127), orbit:YAW,PITCH (1/100 degree),
    /// distance_fine:V, size_fine:V (-32768...32767), curvature:N, dim:N (permille,
    /// 0...1000), hands:0|1, or wait:MS (pause before the next command); `*N` repeats the item.
    static func parseControlSpec(_ spec: String) throws -> [DebugControlStep] {
        try spec.split(separator: ";").flatMap { rawItem -> [DebugControlStep] in
            let item = rawItem.trimmingCharacters(in: .whitespaces)
            let repeatParts = item.split(separator: "*", maxSplits: 1)
            var repeatCount = 1
            if repeatParts.count == 2 {
                guard let n = Int(repeatParts[1].trimmingCharacters(in: .whitespaces)), (1...10_000).contains(n) else {
                    throw OptionError("--debug-control-on-connect: '\(item)': repeat count must be 1...10000")
                }
                repeatCount = n
            }
            let parts = repeatParts.first.map { $0.split(separator: ":", maxSplits: 1) } ?? []
            let name = parts.first.map { $0.trimmingCharacters(in: .whitespaces).lowercased() } ?? ""
            let args = parts.count > 1 ? parts[1].split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) } : []
            func values<T: FixedWidthInteger>(_ count: Int, _: T.Type) throws -> [T] {
                let parsed = args.compactMap { T($0) }
                guard args.count == count, parsed.count == count else {
                    throw OptionError("--debug-control-on-connect: '\(item)' needs \(count) integer argument(s) in \(T.min)...\(T.max)")
                }
                return parsed
            }
            let command: ControlCommand
            switch name {
            case "wait":
                let ms = try values(1, UInt32.self)[0]
                return Array(repeating: .wait(milliseconds: Int(ms)), count: repeatCount)
            case "recenter": _ = try values(0, Int8.self); command = .recenter
            case "reset": _ = try values(0, Int8.self); command = .reset
            case "translate": let v = try values(2, Int8.self); command = .translate(dx: v[0], dy: v[1])
            case "distance": command = .distance(steps: try values(1, Int8.self)[0])
            case "size": command = .size(steps: try values(1, Int8.self)[0])
            case "orbit": let v = try values(2, Int16.self); command = .orbit(yawCentidegrees: v[0], pitchCentidegrees: v[1])
            case "distance_fine": command = .distanceFine(try values(1, Int16.self)[0])
            case "size_fine": command = .sizeFine(try values(1, Int16.self)[0])
            case "hands":
                let v = try values(1, UInt8.self)[0]
                guard v <= 1 else { throw OptionError("--debug-control-on-connect: '\(item)' must be hands:0 or hands:1") }
                command = .setHandGestures(v == 1)
            case "curvature", "dim":
                let v = try values(1, UInt16.self)[0]
                guard v <= ControlCommand.permilleMax else {
                    throw OptionError("--debug-control-on-connect: '\(item)' must be 0...1000 (permille)")
                }
                command = name == "curvature" ? .setCurvature(v) : .setDim(v)
            default: throw OptionError("--debug-control-on-connect: unknown command '\(name)'")
            }
            return Array(repeating: .send(command), count: repeatCount)
        }
    }
}

/// One item of --debug-control-on-connect.
enum DebugControlStep: Equatable {
    case send(ControlCommand)
    case wait(milliseconds: Int)
}

struct OptionError: Error, CustomStringConvertible {
    let description: String
    init(_ d: String) { description = d }
}
