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
    /// CONTROL commands sent to every client right after it connects (testing).
    var controlOnConnect: [ControlCommand] = []

    static let usage = """
    Usage: quest-mirror-sender [options]

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
      --debug-control-on-connect SPEC
                               Testing: send these CONTROL commands to each client on
                               connect, e.g. "move:1,0;distance:-2;size:1;recenter;reset".
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

    /// Parses `cmd[:args];...` with cmd one of recenter, reset, move:DX,DY,
    /// distance:STEPS, size:STEPS (values -128...127).
    static func parseControlSpec(_ spec: String) throws -> [ControlCommand] {
        try spec.split(separator: ";").map { item in
            let parts = item.trimmingCharacters(in: .whitespaces).split(separator: ":", maxSplits: 1)
            let name = parts.first.map { $0.lowercased() } ?? ""
            let args = parts.count > 1 ? parts[1].split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) } : []
            func steps(_ count: Int) throws -> [Int8] {
                let values = args.compactMap { Int8($0) }
                guard args.count == count, values.count == count else {
                    throw OptionError("--debug-control-on-connect: '\(item)' needs \(count) integer argument(s) in -128...127")
                }
                return values
            }
            switch name {
            case "recenter": _ = try steps(0); return .recenter
            case "reset": _ = try steps(0); return .reset
            case "move": let v = try steps(2); return .move(dx: v[0], dy: v[1])
            case "distance": return .distance(steps: try steps(1)[0])
            case "size": return .size(steps: try steps(1)[0])
            default: throw OptionError("--debug-control-on-connect: unknown command '\(name)'")
            }
        }
    }
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
