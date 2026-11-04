import Foundation
import MirrorProtocol

struct ViewerOptions {
    var host = "127.0.0.1"
    var port: UInt16 = WireProtocol.defaultPort
    var headless = false
    var snapshotPath: String?
    var exitAfterSnapshot = false
    var durationSeconds: Double?

    static let usage = """
    Usage: quest-mirror-viewer [options]

      --host HOST              Sender address (default 127.0.0.1).
      --port N                 Sender port (default 7777).
      --headless               No window: decode and print stats only.
      --snapshot PATH.png      Write the first decoded frame as PNG.
      --exit-after-snapshot    Exit once the snapshot is written.
      --duration S             Exit after S seconds.
      -h, --help               Show this help.
    """

    static func parse(_ args: [String]) throws -> ViewerOptions {
        var o = ViewerOptions()
        var it = args.makeIterator()
        func value(_ name: String) throws -> String {
            guard let v = it.next() else { throw OptionError("\(name) needs a value") }
            return v
        }
        while let arg = it.next() {
            switch arg {
            case "--host": o.host = try value(arg)
            case "--port":
                let s = try value(arg)
                guard let p = UInt16(s), p > 0 else { throw OptionError("--port: invalid value '\(s)'") }
                o.port = p
            case "--headless": o.headless = true
            case "--snapshot": o.snapshotPath = try value(arg)
            case "--exit-after-snapshot": o.exitAfterSnapshot = true
            case "--duration":
                let s = try value(arg)
                guard let d = Double(s), d > 0 else { throw OptionError("--duration: invalid value '\(s)'") }
                o.durationSeconds = d
            case "-h", "--help":
                print(usage)
                exit(0)
            default:
                throw OptionError("unknown option '\(arg)'")
            }
        }
        if o.exitAfterSnapshot && o.snapshotPath == nil {
            throw OptionError("--exit-after-snapshot requires --snapshot")
        }
        return o
    }
}

struct OptionError: Error, CustomStringConvertible {
    let description: String
    init(_ d: String) { description = d }
}

func log(_ message: String) {
    print("[\(DateFormatter.logFormatter.string(from: Date()))] \(message)")
}

extension DateFormatter {
    static let logFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()
}
