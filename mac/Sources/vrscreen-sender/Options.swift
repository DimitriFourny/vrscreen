import Foundation
import MirrorProtocol

struct SenderOptions {
    enum Source: String { case screen, test }

    var source: Source = .screen
    var displayIndex: Int?
    var port: UInt16 = WireProtocol.defaultPort
    /// UDP port for DISCOVER/ANNOUNCE; nil with --no-discovery.
    var discoveryPort: UInt16? = DiscoveryMessage.defaultPort
    var fps = 60
    var maxWidth = 3840
    var codec: VideoCodec = .hevc
    var bitrateMbps = 30.0
    var keyframeIntervalSeconds = 5.0
    var maxFramesInFlight = 3
    var width = 1920
    var height = 1080
    var hotkeys = true
    var mouse = true
    var menu = true
    /// Headset on USB for the menu's travel-mode items (scripts/mirror.sh passes it).
    var adbSerial: String?
    /// Degrees of ORBIT per point of mouse movement.
    var mouseDegreesPerPoint = 0.1
    /// Multiplier on the scroll → DISTANCE_FINE / SIZE_FINE scaling.
    var scrollSensitivity = 1.0
    /// Steps sent to every client right after it connects (testing).
    var controlOnConnect: [DebugControlStep] = []

    static let usage = """
    Usage: vrscreen-sender [options]

      --source screen|test     Frame source (default screen). "test" is a synthetic
                               pattern that needs no Screen Recording permission.
      --display N              Display index from the list printed at startup
                               (default: the main display).
      --port N                 TCP listen port (default 7777).
      --discovery-port N       UDP port answering LAN discovery (default 7778).
      --no-discovery           Do not answer LAN discovery.
      --fps N                  Target frame rate (default 60).
      --max-width N            Scale the display down to fit this width (default 3840).
      --codec hevc|h264        Video codec (default hevc).
      --bitrate MBPS           Average bitrate in Mbit/s (default 30).
      --keyframe-interval S    Max seconds between keyframes (default 5).
      --max-in-flight N        For clients that send FRAME_ACK: max frames sent but
                               not yet consumed by the client (default 3).
      --width N --height N     Test source size (default 1920x1080).
      --no-hotkeys             Do not register the global ⌃⌥⌘ screen-placement shortcuts.
      --no-mouse               Do not use ⌃⌥⌘ + mouse (orbit) and ⌃⌥⌘ + scroll (zoom).
      --no-menu                No menu-bar icon (curvature, dimming, re-center, quit).
      --adb-serial S           Headset on USB (adb serial) for the menu's travel-mode
                               items; without it they show "headset not on USB".
      --mouse-sensitivity DEG  Orbit degrees per point of mouse movement (default 0.1).
      --scroll-sensitivity X   Scroll zoom multiplier (default 1; a wheel notch is a
                               quarter DISTANCE/SIZE step).
      --debug-control-on-connect SPEC
                               Testing: send these CONTROL commands to each client on
                               connect, e.g. "translate:1,0;orbit:500,-250;distance:-2;
                               distance_fine:25;size:1;size_fine:-50;recenter;reset".
      -h, --help               Show this help.
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
            guard let v = Double(s), v > 0 else { throw OptionError("\(name): invalid value '\(s)'") }
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
            case "--fps": o.fps = try int(arg)
            case "--max-width": o.maxWidth = try int(arg, min: 16)
            case "--codec":
                switch try value(arg).lowercased() {
                case "hevc", "h265": o.codec = .hevc
                case "h264", "avc": o.codec = .h264
                default: throw OptionError("--codec must be hevc or h264")
                }
            case "--bitrate": o.bitrateMbps = try double(arg)
            case "--keyframe-interval": o.keyframeIntervalSeconds = try double(arg)
            case "--max-in-flight": o.maxFramesInFlight = try int(arg)
            case "--width": o.width = try int(arg, min: 16) & ~1
            case "--height": o.height = try int(arg, min: 16) & ~1
            case "--no-hotkeys": o.hotkeys = false
            case "--no-mouse": o.mouse = false
            case "--no-menu": o.menu = false
            case "--adb-serial": o.adbSerial = try value(arg)
            case "--mouse-sensitivity": o.mouseDegreesPerPoint = try double(arg)
            case "--scroll-sensitivity": o.scrollSensitivity = try double(arg)
            case "--debug-control-on-connect": o.controlOnConnect = try parseControlSpec(try value(arg))
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

func log(_ message: String) {
    let f = DateFormatter.logFormatter
    print("[\(f.string(from: Date()))] \(message)")
}

extension DateFormatter {
    static let logFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()
}
