import Foundation

/// Logs a line to stdout and to the log file (`~/Library/Logs/VRScreen/vrscreen.log` once
/// `LogFile.shared` is opened). Any thread.
func log(_ message: String) {
    let now = Date()
    print("[\(DateFormatter.logFormatter.string(from: now))] \(message)")
    LogFile.shared.write("[\(DateFormatter.logFileFormatter.string(from: now))] \(message)\n")
}

extension DateFormatter {
    static let logFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()

    static let logFileFormatter: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd HH:mm:ss.SSS"
        return f
    }()
}

/// Append-only log file with size-based rotation: when it grows past `maxBytes`, it becomes
/// `<name>.1.log` (replacing the previous one) and a new file starts, so at most twice
/// `maxBytes` are kept. Writes before `open` are dropped. Thread-safe.
final class LogFile {
    static let shared = LogFile()
    static let maxBytes = 5 * 1024 * 1024

    static var standardURL: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/VRScreen/vrscreen.log")
    }

    private let lock = NSLock()
    private var handle: FileHandle?
    private var size = 0
    private(set) var url: URL?
    private var maxBytes = LogFile.maxBytes

    /// Opens (creating directories and the file) for appending.
    func open(_ url: URL, maxBytes: Int = LogFile.maxBytes) throws {
        lock.lock()
        defer { lock.unlock() }
        self.maxBytes = maxBytes
        self.url = url
        try openLocked(url)
    }

    func write(_ line: String) {
        lock.lock()
        defer { lock.unlock() }
        guard let handle, let url else { return }
        let data = Data(line.utf8)
        handle.write(data)
        size += data.count
        if size >= maxBytes {
            try? handle.close()
            self.handle = nil
            let old = Self.rotatedURL(for: url)
            try? FileManager.default.removeItem(at: old)
            try? FileManager.default.moveItem(at: url, to: old)
            try? openLocked(url)
        }
    }

    /// `vrscreen.log` → `vrscreen.1.log`.
    static func rotatedURL(for url: URL) -> URL {
        url.deletingPathExtension().appendingPathExtension("1").appendingPathExtension(url.pathExtension)
    }

    private func openLocked(_ url: URL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        // O_APPEND: other senders (the app and a dev run) may write the same file.
        let fd = Darwin.open(url.path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0o644)
        guard fd >= 0 else { throw CocoaError(.fileWriteUnknown, userInfo: [NSFilePathErrorKey: url.path]) }
        var info = stat()
        size = fstat(fd, &info) == 0 ? Int(info.st_size) : 0
        handle = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
    }
}

/// Facts about this build: version and bundled resources (VRScreen.app), or a bare dev binary.
enum AppInfo {
    static let bundleIdentifier = "com.df0.vrscreen.mac"
    /// Package name of the headset app.
    static let headsetPackage = "com.df0.vrscreen"

    /// Running from VRScreen.app (not a bare `vrscreen-sender` binary).
    static var isAppBundle: Bool { Bundle.main.bundleIdentifier == bundleIdentifier }

    /// CFBundleShortVersionString of VRScreen.app; "dev" for a bare binary.
    static var version: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "dev"
    }

    /// The headset app built together with VRScreen.app (same version), if bundled.
    static var bundledHeadsetAPK: URL? {
        guard isAppBundle else { return nil }
        return Bundle.main.url(forResource: "vrscreen-headset", withExtension: "apk")
    }
}
