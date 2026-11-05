import Foundation
import IOKit

/// Headset travel-mode state as read over adb.
enum TravelModeState: Equatable {
    case on
    case off
    /// adb works but the dumpsys output has no recognizable `* Travel Mode:` line.
    case formatChanged
    /// The headset is not reachable over USB (not attached, unauthorized, timeout).
    case notOnUSB
    /// No adb executable.
    case adbMissing
    /// --no-adb.
    case usbOff

    var menuTitle: String {
        switch self {
        case .on: "Travel mode: On"
        case .off: "Travel mode: Off"
        case .formatChanged: "Travel mode: unavailable (dumpsys format changed)"
        case .notOnUSB: "Travel mode: unknown — headset not on USB"
        case .adbMissing: "Travel mode: unknown — adb not found"
        case .usbOff: "Travel mode: unknown — USB off (--no-adb)"
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

/// Runs the adb client. Every child gets `ADB_MDNS=0`: adb 37.0.1's mDNS backend crashes
/// the server, and a server started by one of these calls inherits the setting.
struct Adb: Equatable {
    static let timeout: TimeInterval = 3
    /// Where to send people who have no adb.
    static let installCommand = "brew install --cask android-platform-tools"
    static let downloadPage = URL(string: "https://developer.android.com/tools/releases/platform-tools")!

    let executable: URL

    /// `adb` from PATH, ANDROID_HOME / ANDROID_SDK_ROOT, then the usual install locations
    /// (apps started from Finder get a minimal PATH).
    static func locate(environment: [String: String] = ProcessInfo.processInfo.environment) -> Adb? {
        let home = environment["HOME"] ?? NSHomeDirectory()
        var candidates = (environment["PATH"] ?? "").split(separator: ":").map(String.init)
        for key in ["ANDROID_HOME", "ANDROID_SDK_ROOT"] {
            if let sdk = environment[key], !sdk.isEmpty { candidates.append("\(sdk)/platform-tools") }
        }
        candidates += ["\(home)/Library/Android/sdk/platform-tools", "/opt/homebrew/bin", "/usr/local/bin",
                       "/opt/homebrew/share/android-commandlinetools/platform-tools"]
        for dir in candidates where !dir.isEmpty {
            let url = URL(fileURLWithPath: dir).appendingPathComponent("adb")
            if FileManager.default.isExecutableFile(atPath: url.path) { return Adb(executable: url) }
        }
        return nil
    }

    enum RunError: Error, CustomStringConvertible, Equatable {
        case timedOut(TimeInterval)
        case failed(status: Int32, output: String)
        case launch(String)

        var description: String {
            switch self {
            case .timedOut(let seconds): "timed out after \(Int(seconds)) s"
            case .failed(let status, let output): "exit \(status): \(output.trimmingCharacters(in: .whitespacesAndNewlines))"
            case .launch(let error): "cannot run adb: \(error)"
            }
        }
    }

    /// Runs `adb <arguments>` and waits for it (at most `timeout`; only this client process
    /// is terminated on a timeout, never the server). Blocking: not on the main thread.
    func run(_ arguments: [String], timeout: TimeInterval = Adb.timeout) -> Result<String, RunError> {
        let process = Process()
        process.executableURL = executable
        process.arguments = arguments
        var environment = ProcessInfo.processInfo.environment
        environment["ADB_MDNS"] = "0"
        process.environment = environment
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        process.standardInput = FileHandle.nullDevice

        // Collect output as it comes (a full pipe would block adb). `start-server` forks a
        // daemon that may keep the pipe open, so completion is the client's exit, not EOF.
        let output = OutputBuffer()
        let exited = DispatchSemaphore(value: 0)
        pipe.fileHandleForReading.readabilityHandler = { handle in
            let data = handle.availableData
            if data.isEmpty { handle.readabilityHandler = nil } else { output.append(data) }
        }
        process.terminationHandler = { _ in exited.signal() }
        do {
            try process.run()
        } catch {
            pipe.fileHandleForReading.readabilityHandler = nil
            return .failure(.launch("\(error)"))
        }
        if exited.wait(timeout: .now() + timeout) == .timedOut {
            process.terminate()
            _ = exited.wait(timeout: .now() + 1)
            pipe.fileHandleForReading.readabilityHandler = nil
            return .failure(.timedOut(timeout))
        }
        // Whatever is still buffered in the pipe.
        let rest = (try? pipe.fileHandleForReading.readToEnd()) ?? nil
        pipe.fileHandleForReading.readabilityHandler = nil
        if let rest { output.append(rest) }
        let text = output.string
        if process.terminationReason == .exit && process.terminationStatus == 0 { return .success(text) }
        return .failure(.failed(status: process.terminationStatus, output: text))
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
}

private final class OutputBuffer: @unchecked Sendable {
    private let lock = NSLock()
    private var data = Data()

    func append(_ chunk: Data) {
        lock.lock()
        data.append(chunk)
        lock.unlock()
    }

    var string: String {
        lock.lock()
        defer { lock.unlock() }
        return String(decoding: data, as: UTF8.self)
    }
}

/// One line of `adb devices -l`.
struct AdbDevice: Equatable {
    enum State: Equatable {
        case device
        case unauthorized
        /// Being authorized right now (transient).
        case authorizing
        case offline
        /// "no permissions" (USB access denied).
        case noPermissions
        case other(String)
    }

    /// Horizon OS `ro.product.device` names, for headsets whose model string lacks "Quest".
    static let questProducts: Set<String> = ["eureka", "panther", "seacliff", "hollywood", "monterey"]

    var serial: String
    var state: State
    /// "Quest 3" (adb reports `model:Quest_3`); unknown while unauthorized.
    var model: String?
    var product: String?

    var isQuest: Bool {
        model?.localizedCaseInsensitiveContains("quest") == true || product.map(Self.questProducts.contains) == true
    }

    var displayName: String { model ?? "Android device" }

    /// Parses `adb devices -l` (the header, daemon messages and blank lines are skipped).
    static func parse(devicesOutput output: String) -> [AdbDevice] {
        output.split(whereSeparator: \.isNewline).compactMap { rawLine in
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            guard !line.isEmpty, !line.hasPrefix("*"), !line.hasPrefix("List of devices"),
                  !line.hasPrefix("adb ") else { return nil }
            let tokens = line.split(whereSeparator: \.isWhitespace).map(String.init)
            guard tokens.count >= 2 else { return nil }
            let state: State
            switch tokens[1] {
            case "device": state = .device
            case "unauthorized": state = .unauthorized
            case "authorizing": state = .authorizing
            case "offline": state = .offline
            case "no" where tokens.count > 2 && tokens[2].hasPrefix("permissions"): state = .noPermissions
            default: state = .other(tokens[1])
            }
            func field(_ name: String) -> String? {
                tokens.first { $0.hasPrefix(name + ":") }.map { String($0.dropFirst(name.count + 1)) }
            }
            return AdbDevice(serial: tokens[0], state: state,
                             model: field("model")?.replacingOccurrences(of: "_", with: " "),
                             product: field("product"))
        }
    }
}

/// Which adb device is the headset, and in what state.
enum HeadsetSelection: Equatable {
    case none
    /// Usable; `others` = other adb devices attached.
    case ready(AdbDevice, others: Int)
    /// Waiting for "Allow USB debugging" in the headset.
    case unauthorized(AdbDevice)
    /// Being authorized (transient).
    case authorizing(AdbDevice)
    /// offline / no permissions: replug the cable.
    case offline(AdbDevice)
    /// Several ready devices and none looks like a Quest.
    case ambiguous(count: Int)

    /// With `preferredSerial` (--adb-serial) only that device counts. Otherwise a ready
    /// Quest wins, then a device waiting for authorization (it may be the headset: its model
    /// is unknown until then), a broken one, one being authorized, and finally the only
    /// ready device whatever its model.
    static func select(_ devices: [AdbDevice], preferredSerial: String?) -> HeadsetSelection {
        let candidates = preferredSerial.map { serial in devices.filter { $0.serial == serial } } ?? devices
        let ready = candidates.filter { $0.state == .device }
        let others = devices.count - 1
        if let quest = ready.filter(\.isQuest).min(by: { $0.serial < $1.serial }) {
            return .ready(quest, others: others)
        }
        if let device = candidates.first(where: { $0.state == .unauthorized }) { return .unauthorized(device) }
        if let device = candidates.first(where: { $0.state == .offline || $0.state == .noPermissions }) {
            return .offline(device)
        }
        if let device = candidates.first(where: { $0.state == .authorizing }) { return .authorizing(device) }
        if ready.count == 1 { return .ready(ready[0], others: others) }
        if ready.count > 1 { return .ambiguous(count: ready.count) }
        return .none
    }
}

/// `adb reverse --list` entries and what to do about ours.
enum AdbReverse {
    struct Entry: Equatable {
        var remote: String
        var local: String
    }

    enum Decision: Equatable {
        /// `tcp:P tcp:P` exists already.
        case present
        case create
        /// The headset's port is forwarded elsewhere; replace it.
        case replace(existingLocal: String)
    }

    /// Lines like `UsbFfs tcp:7777 tcp:7777` (the first column varies with the adb version).
    static func parse(listOutput output: String) -> [Entry] {
        output.split(whereSeparator: \.isNewline).compactMap { line in
            let tokens = line.split(whereSeparator: \.isWhitespace).map(String.init)
            guard tokens.count >= 2 else { return nil }
            let remote = tokens[tokens.count - 2], local = tokens[tokens.count - 1]
            guard remote.contains(":"), local.contains(":") else { return nil }
            return Entry(remote: remote, local: local)
        }
    }

    static func decide(_ entries: [Entry], port: UInt16) -> Decision {
        let spec = "tcp:\(port)"
        guard let entry = entries.first(where: { $0.remote == spec }) else { return .create }
        return entry.local == spec ? .present : .replace(existingLocal: entry.local)
    }
}

/// Whether the headset app is installed, and which version.
enum HeadsetAppState: Equatable {
    case unknown
    case notInstalled
    case installed(version: String)

    /// `versionName=0.2.0` from `dumpsys package <pkg> | grep versionName`; empty → not installed.
    static func parse(dumpsysVersionLines output: String) -> HeadsetAppState {
        for line in output.split(whereSeparator: \.isNewline) {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.hasPrefix("versionName=") {
                return .installed(version: String(trimmed.dropFirst("versionName=".count)))
            }
        }
        return .notInstalled
    }
}

/// What the Headset menu offers for the bundled headset app.
enum HeadsetAppAction: Equatable {
    case install(version: String)
    case update(from: String, to: String)
    /// Same (or newer) version installed: offered as "Reinstall".
    case reinstall(version: String)

    /// nil without a bundled APK (bare dev binary) or while the installed state is unknown.
    static func offer(installed: HeadsetAppState, bundled: String?) -> HeadsetAppAction? {
        guard let bundled else { return nil }
        switch installed {
        case .unknown: return nil
        case .notInstalled: return .install(version: bundled)
        case .installed(let version):
            if let order = compareVersions(version, bundled), order == .orderedAscending {
                return .update(from: version, to: bundled)
            }
            return .reinstall(version: bundled)
        }
    }

    var isNeeded: Bool {
        switch self {
        case .install, .update: true
        case .reinstall: false
        }
    }

    var menuTitle: String {
        switch self {
        case .install(let version): "Install headset app (\(version))"
        case .update(let from, let to): "Update headset app (\(from) → \(to))"
        case .reinstall(let version): "Reinstall headset app (\(version))"
        }
    }
}

/// Numeric dotted-version comparison ("0.10.0" > "0.9.1"; missing parts count as 0). nil if
/// either side has no leading number ("dev").
func compareVersions(_ a: String, _ b: String) -> ComparisonResult? {
    func parts(_ s: String) -> [Int]? {
        // "0.3.0-beta" → [0, 3, 0]
        let numbers = s.prefix { $0.isNumber || $0 == "." }.split(separator: ".").compactMap { Int($0) }
        return numbers.isEmpty ? nil : numbers
    }
    guard let x = parts(a), let y = parts(b) else { return nil }
    for i in 0..<max(x.count, y.count) {
        let l = i < x.count ? x[i] : 0, r = i < y.count ? y[i] : 0
        if l != r { return l < r ? .orderedAscending : .orderedDescending }
    }
    return .orderedSame
}

/// `adb install` output: nil on success, else the failure reason ("INSTALL_FAILED_…").
func adbInstallFailure(_ output: String) -> String? {
    let lines = output.split(whereSeparator: \.isNewline).map { $0.trimmingCharacters(in: .whitespaces) }
    if lines.contains("Success") { return nil }
    if let failure = lines.first(where: { $0.hasPrefix("Failure") || $0.contains("INSTALL_FAILED") || $0.hasPrefix("adb: failed") }) {
        if let open = failure.firstIndex(of: "["), let close = failure[open...].firstIndex(of: "]") {
            let inner = failure[failure.index(after: open)..<close]
            return String(inner.split(separator: ":").first ?? inner).trimmingCharacters(in: .whitespaces)
        }
        return failure
    }
    return lines.last ?? "no output"
}

/// adb commands for one headset (`adb -s <serial> …`). Blocking: call off the main thread.
struct HeadsetAdb {
    let adb: Adb
    let serial: String

    func run(_ arguments: [String], timeout: TimeInterval = Adb.timeout) -> Result<String, Adb.RunError> {
        adb.run(["-s", serial] + arguments, timeout: timeout)
    }

    func readTravelMode() -> TravelModeState {
        // Filter on the device so only the relevant line crosses the USB link.
        switch run(["shell", "dumpsys tracking | grep -F 'Travel Mode:'"]) {
        case .success(let output): TravelModeState.parse(dumpsysTracking: output)
        case .failure(.failed(let status, let output)) where status == 1 && output.isEmpty: .formatChanged
        case .failure: .notOnUSB
        }
    }

    /// Opens Quick Settings in the headset (the user then taps the Travel mode tile).
    func openQuickSettings() -> Result<String, Adb.RunError> {
        run(["shell", "am", "start", "-a", "com.oculus.action.QUICK_SETTINGS"])
    }

    func headsetApp() -> HeadsetAppState {
        switch run(["shell", "dumpsys package \(AppInfo.headsetPackage) | grep versionName"]) {
        case .success(let output): HeadsetAppState.parse(dumpsysVersionLines: output)
        case .failure(.failed(let status, let output)) where status == 1 && output.isEmpty: .notInstalled
        case .failure: .unknown
        }
    }
}

/// Meta USB devices (vendor 0x2833) seen by IOKit, whether or not adb sees them: a Quest
/// without Developer Mode / USB debugging still shows up here.
enum MetaUSB {
    static let vendorID = 0x2833

    /// Product name of the first attached Meta device ("Quest 3"), or nil. (IOUSBHostDevice
    /// does not match on idVendor alone, so the few USB devices are filtered here.)
    static func attachedProductName() -> String? {
        guard let matching = IOServiceMatching("IOUSBHostDevice") else { return nil }
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) == KERN_SUCCESS else { return nil }
        defer { IOObjectRelease(iterator) }
        while case let service = IOIteratorNext(iterator), service != 0 {
            defer { IOObjectRelease(service) }
            func property(_ key: String) -> AnyObject? {
                IORegistryEntryCreateCFProperty(service, key as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue()
            }
            guard (property("idVendor") as? NSNumber)?.intValue == vendorID else { continue }
            return property("USB Product Name") as? String ?? "Meta headset"
        }
        return nil
    }
}
