import Foundation

/// Headset travel-mode state as read over adb.
enum TravelModeState: Equatable {
    case on
    case off
    /// adb works but the dumpsys output has no recognizable `* Travel Mode:` line.
    case formatChanged
    /// The headset is not reachable over USB (no serial, not attached, unauthorized, timeout).
    case notOnUSB
    /// No adb executable.
    case adbMissing
    /// The adb server is not running; the sender does not start it.
    case adbServerDown

    var menuTitle: String {
        switch self {
        case .on: "Travel mode: On"
        case .off: "Travel mode: Off"
        case .formatChanged: "Travel mode: unavailable (dumpsys format changed)"
        case .notOnUSB: "Travel mode: unknown — headset not on USB"
        case .adbMissing: "Travel mode: unavailable — adb not found"
        case .adbServerDown: "Travel mode: unknown — adb server not running"
        }
    }

    /// Whether the headset answered over adb (so Quick Settings can be opened).
    var headsetReachable: Bool {
        switch self {
        case .on, .off, .formatChanged: true
        case .notOnUSB, .adbMissing, .adbServerDown: false
        }
    }

    /// Parses `dumpsys tracking` (Horizon OS): the line ` * Travel Mode: true|false`.
    /// The unrelated `TravelMode: off` line elsewhere in the dump is ignored.
    static func parse(dumpsysTracking output: String) -> TravelModeState {
        for line in output.split(whereSeparator: \.isNewline) {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            guard trimmed.hasPrefix("* Travel Mode:") else { continue }
            switch trimmed.dropFirst("* Travel Mode:".count).trimmingCharacters(in: .whitespaces).lowercased() {
            case "true": return .on
            case "false": return .off
            default: return .formatChanged
            }
        }
        return .formatChanged
    }
}

/// Minimal adb client for the headset on USB: read the travel-mode state and open
/// Quick Settings. Only `adb -s <serial> shell …` commands run, and only while an adb
/// server is already running (an adb client would otherwise start one; the sender never
/// starts or stops the server). Every child gets `ADB_MDNS=0` (adb 37.0.1's mDNS backend
/// crashes the server).
struct HeadsetAdb {
    static let timeout: TimeInterval = 3

    let executable: URL
    let serial: String

    /// `adb` from PATH, then the usual install locations.
    static func locateExecutable(environment: [String: String] = ProcessInfo.processInfo.environment) -> URL? {
        let pathDirs = (environment["PATH"] ?? "").split(separator: ":").map(String.init)
        let home = environment["HOME"] ?? NSHomeDirectory()
        let candidates = pathDirs + ["\(home)/Library/Android/sdk/platform-tools", "/opt/homebrew/bin", "/usr/local/bin"]
        for dir in candidates where !dir.isEmpty {
            let url = URL(fileURLWithPath: dir).appendingPathComponent("adb")
            if FileManager.default.isExecutableFile(atPath: url.path) { return url }
        }
        return nil
    }

    enum RunError: Error, CustomStringConvertible {
        case serverNotRunning
        case timedOut
        case failed(status: Int32, output: String)
        case launch(Error)

        var description: String {
            switch self {
            case .serverNotRunning: "adb server not running"
            case .timedOut: "timed out after \(HeadsetAdb.timeout) s"
            case .failed(let status, let output): "exit \(status): \(output.trimmingCharacters(in: .whitespacesAndNewlines))"
            case .launch(let error): "cannot run adb: \(error)"
            }
        }
    }

    /// Reads the travel-mode state; `completion` runs on an arbitrary queue.
    func readTravelMode(completion: @escaping (TravelModeState) -> Void) {
        // Filter on the device so only the relevant line crosses the USB link.
        run(["shell", "dumpsys tracking | grep -F 'Travel Mode:'"]) { result in
            switch result {
            case .success(let output):
                completion(TravelModeState.parse(dumpsysTracking: output))
            case .failure(.failed(let status, let output)) where status == 1 && output.isEmpty:
                completion(.formatChanged) // grep matched nothing: the line is gone
            case .failure(.serverNotRunning):
                completion(.adbServerDown)
            case .failure:
                completion(.notOnUSB)
            }
        }
    }

    /// Opens Quick Settings in the headset (the user then taps the Travel mode tile).
    func openQuickSettings(completion: @escaping (Result<String, RunError>) -> Void) {
        run(["shell", "am", "start", "-a", "com.oculus.action.QUICK_SETTINGS"], completion: completion)
    }

    /// Whether something listens on the adb server port (ANDROID_ADB_SERVER_PORT, default
    /// 5037) on localhost. A refused connection returns at once.
    static func serverIsRunning(environment: [String: String] = ProcessInfo.processInfo.environment) -> Bool {
        let port = environment["ANDROID_ADB_SERVER_PORT"].flatMap { UInt16($0) } ?? 5037
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { return false }
        defer { close(fd) }
        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        addr.sin_addr = in_addr(s_addr: inet_addr("127.0.0.1"))
        return withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) == 0
            }
        }
    }

    private func run(_ arguments: [String], completion: @escaping (Result<String, RunError>) -> Void) {
        guard Self.serverIsRunning() else { return completion(.failure(.serverNotRunning)) }
        let process = Process()
        process.executableURL = executable
        process.arguments = ["-s", serial] + arguments
        var environment = ProcessInfo.processInfo.environment
        environment["ADB_MDNS"] = "0"
        process.environment = environment
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        process.standardInput = FileHandle.nullDevice

        let lock = NSLock()
        var finished = false
        func finish(_ result: Result<String, RunError>) {
            lock.lock()
            defer { lock.unlock() }
            guard !finished else { return }
            finished = true
            completion(result)
        }

        do {
            try process.run()
        } catch {
            finish(.failure(.launch(error)))
            return
        }
        // Read while it runs (a full pipe would block adb), then collect the status.
        DispatchQueue.global().async {
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            let output = String(decoding: data, as: UTF8.self)
            if process.terminationReason == .exit && process.terminationStatus == 0 {
                finish(.success(output))
            } else {
                finish(.failure(.failed(status: process.terminationStatus, output: output)))
            }
        }
        DispatchQueue.global().asyncAfter(deadline: .now() + Self.timeout) {
            // Only this adb client process is terminated, never the server.
            if process.isRunning { process.terminate() }
            finish(.failure(.timedOut))
        }
    }
}
