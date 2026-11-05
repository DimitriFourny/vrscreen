import Foundation

/// What the sender knows about the headset on USB, for the menu and the problem list.
struct UsbStatus: Equatable {
    enum Link: Equatable {
        /// --no-adb.
        case off
        case adbMissing
        /// The adb server does not start (even after kill-server), or `adb devices` fails.
        case adbBroken(String)
        case noHeadset
        case unauthorized(serial: String)
        case authorizing
        case offline(serial: String)
        case ambiguous(count: Int)
        case connected(serial: String, model: String, otherDevices: Int)
    }

    var link: Link
    /// Product name of a Meta USB device that IOKit sees while adb does not (polled while
    /// no headset is usable over adb).
    var metaUSBDevice: String?
    /// The adb server stopped and was restarted 3 times within 2 minutes.
    var serverUnstable = false
    var app = HeadsetAppState.unknown
    /// Version of the APK inside VRScreen.app, if any.
    var bundledVersion: String?
    var installing = false
    /// "Installed 0.3.0" / "Install failed: …", until the headset leaves.
    var installResult: String?

    var isConnected: Bool {
        if case .connected = link { return true }
        return false
    }

    /// The Headset ▸ submenu's first line.
    var linkText: String {
        switch link {
        case .off: "USB off (--no-adb)"
        case .adbMissing: "USB unavailable: adb not found"
        case .adbBroken: "USB unavailable: adb isn't working"
        case .noHeadset: metaUSBDevice.map { "\($0) on USB without USB debugging" } ?? "No headset on USB"
        case .unauthorized: "Headset on USB: waiting for “Allow USB debugging”"
        case .authorizing: "Headset on USB: authorizing…"
        case .offline: "Headset on USB: offline (replug the cable)"
        case .ambiguous(let count): "\(count) adb devices, none is a Quest (use --adb-serial)"
        case .connected(let serial, let model, let others):
            "\(model) on USB (\(serial))" + (others > 0 ? ", \(others) other adb device\(others == 1 ? "" : "s")" : "")
        }
    }

    /// "Headset app: 0.2.0 installed · 0.3.0 bundled".
    var appText: String? {
        guard isConnected else { return nil }
        let installed: String
        switch app {
        case .unknown: installed = "checking…"
        case .notInstalled: installed = "not installed"
        case .installed(let version): installed = "\(version) installed"
        }
        return "Headset app: \(installed)" + (bundledVersion.map { " · \($0) bundled" } ?? "")
    }
}

/// The headset over USB, driven by adb: finds the authorized headset (polling every 3 s),
/// starts the adb server when needed (with `ADB_MDNS=0`), sets up `adb reverse tcp:P tcp:P`
/// when the headset appears (and re-checks it on every poll), launches the headset app once
/// per attach (if enabled), installs the bundled APK on request, and removes on exit only
/// the reverse it created. All adb calls run on its own serial queue.
final class UsbHeadsetLink {
    static let pollInterval: TimeInterval = 3
    static let launchActivity = "\(AppInfo.headsetPackage)/android.app.NativeActivity"
    static let installTimeout: TimeInterval = 300
    /// This many server restarts within `restartWindow` count as "keeps dying".
    static let unstableRestarts = 3
    static let restartWindow: TimeInterval = 120
    /// A server that does not start is retried this often.
    static let serverRetryInterval: TimeInterval = 30

    private let port: UInt16
    private let preferredSerial: String?
    private let bundledAPK: URL?
    private let onStatus: (UsbStatus) -> Void
    private let queue = DispatchQueue(label: "vrscreen.adb", qos: .utility)

    // On `queue`.
    private var adb: Adb?
    private var status: UsbStatus
    private var published: UsbStatus?
    private var timer: DispatchSourceTimer?
    private var launchOnAttach: Bool
    private var attachedSerial: String?
    private var serverSeenRunning = false
    private var serverRestarts: [Date] = []
    /// The last start-server failure, and when to try again.
    private var serverError: String?
    private var serverRetryAt: Date?
    private var loggedExistingReverse = false
    private var lastLoggedSelection: String?

    /// The attached headset, for travel-mode queries from other queues.
    private let lock = NSLock()
    private var currentHeadset: HeadsetAdb?

    /// `onStatus` runs on the main queue with every change.
    init(port: UInt16, preferredSerial: String?, bundledAPK: URL?, launchOnAttach: Bool,
         onStatus: @escaping (UsbStatus) -> Void) {
        self.port = port
        self.preferredSerial = preferredSerial
        self.bundledAPK = bundledAPK
        self.launchOnAttach = launchOnAttach
        self.onStatus = onStatus
        status = UsbStatus(link: .noHeadset, bundledVersion: bundledAPK != nil ? AppInfo.version : nil)
        atexit { UsbHeadsetLink.removeCreatedReverse() }
    }

    func start() {
        queue.async { [self] in
            adb = Adb.locate()
            log(adb.map { "usb: using \($0.executable.path) (ADB_MDNS=0)" }
                ?? "usb: adb not found (\(Adb.installCommand)); USB disabled, Wi-Fi works")
            let timer = DispatchSource.makeTimerSource(queue: queue)
            timer.schedule(deadline: .now(), repeating: Self.pollInterval, leeway: .milliseconds(200))
            timer.setEventHandler { [weak self] in self?.poll() }
            timer.resume()
            self.timer = timer
        }
    }

    func setLaunchOnAttach(_ on: Bool) {
        queue.async { [self] in launchOnAttach = on }
    }

    /// The attached, authorized headset (any thread).
    var headset: HeadsetAdb? {
        lock.lock()
        defer { lock.unlock() }
        return currentHeadset
    }

    /// Installs the bundled APK (`adb install -r`), then launches the app if enabled.
    func installApp() {
        queue.async { [self] in
            guard let headset, let apk = bundledAPK, !status.installing else { return }
            status.installing = true
            status.installResult = nil
            publish()
            log("usb: installing \(apk.lastPathComponent) (\(AppInfo.version)) on the headset")
            let result = headset.run(["install", "-r", apk.path], timeout: Self.installTimeout)
            let failure: String? = switch result {
            case .success(let output): adbInstallFailure(output)
            case .failure(.failed(_, let output)): adbInstallFailure(output)
            case .failure(let error): error.description
            }
            status.installing = false
            status.app = headset.headsetApp()
            if let failure {
                status.installResult = "Install failed: \(failure)"
                if failure.contains("UPDATE_INCOMPATIBLE") {
                    status.installResult! += " (signed by another build: uninstall VRScreen in the headset first)"
                }
                log("usb: install failed: \(failure)")
            } else {
                status.installResult = "Installed \(AppInfo.version)"
                log("usb: headset app \(AppInfo.version) installed")
                if launchOnAttach { launch(headset, reason: "installed") }
            }
            publish()
        }
    }

    func launchApp() {
        queue.async { [self] in
            guard let headset else { return }
            launch(headset, reason: "menu")
        }
    }

    /// kill-server + start-server (menu "Restart adb"), then a fresh poll.
    func restartAdb() {
        queue.async { [self] in
            guard let adb else { return }
            log("usb: restarting the adb server (menu)")
            _ = adb.run(["kill-server"], timeout: 5)
            serverSeenRunning = false
            serverRestarts.removeAll()
            serverError = nil
            serverRetryAt = nil
            poll()
        }
    }

    // MARK: Polling (on `queue`)

    private func poll() {
        if adb == nil, let found = Adb.locate() {
            adb = found
            log("usb: found \(found.executable.path)")
        }
        guard let adb else {
            status.link = .adbMissing
            status.metaUSBDevice = MetaUSB.attachedProductName()
            return publish()
        }
        if !Adb.serverIsRunning() {
            // While it fails, retry every 30 s (not every poll) and log only new errors.
            if let retryAt = serverRetryAt, Date() < retryAt { return }
            if serverError == nil {
                if serverSeenRunning {
                    serverRestarts.append(Date())
                    log("usb: the adb server stopped; starting it again (ADB_MDNS=0)")
                } else {
                    log("usb: starting the adb server (ADB_MDNS=0)")
                }
            }
            if let error = startServer(adb) {
                if error != serverError { log("usb: adb server does not start: \(error); retrying every 30 s") }
                serverError = error
                serverRetryAt = Date() + Self.serverRetryInterval
                detachIfNeeded()
                status.link = .adbBroken(error)
                status.metaUSBDevice = MetaUSB.attachedProductName()
                return publish()
            }
            if serverError != nil { log("usb: adb server running again") }
        }
        serverError = nil
        serverRetryAt = nil
        serverSeenRunning = true
        serverRestarts.removeAll { Date().timeIntervalSince($0) > Self.restartWindow }
        status.serverUnstable = serverRestarts.count >= Self.unstableRestarts

        let devices: [AdbDevice]
        switch adb.run(["devices", "-l"]) {
        case .success(let output):
            devices = AdbDevice.parse(devicesOutput: output)
        case .failure(let error):
            detachIfNeeded()
            status.link = .adbBroken("adb devices: \(error)")
            return publish()
        }
        let selection = HeadsetSelection.select(devices, preferredSerial: preferredSerial)
        logSelection(selection)
        status.metaUSBDevice = nil
        switch selection {
        case .ready(let device, let others):
            if attachedSerial != device.serial {
                detachIfNeeded()
                attach(HeadsetAdb(adb: adb, serial: device.serial), model: device.displayName)
            } else if let headset {
                ensureReverse(headset)
            }
            status.link = .connected(serial: device.serial, model: device.displayName, otherDevices: others)
        case .unauthorized(let device):
            detachIfNeeded()
            status.link = .unauthorized(serial: device.serial)
        case .authorizing:
            detachIfNeeded()
            status.link = .authorizing
        case .offline(let device):
            detachIfNeeded()
            status.link = .offline(serial: device.serial)
        case .ambiguous(let count):
            detachIfNeeded()
            status.link = .ambiguous(count: count)
        case .none:
            detachIfNeeded()
            status.link = .noHeadset
            status.metaUSBDevice = MetaUSB.attachedProductName()
        }
        publish()
    }

    /// nil once the server runs; tries kill-server + start-server once if start-server fails.
    private func startServer(_ adb: Adb) -> String? {
        if case .success = adb.run(["start-server"], timeout: 10), Adb.serverIsRunning() { return nil }
        _ = adb.run(["kill-server"], timeout: 5)
        switch adb.run(["start-server"], timeout: 10) {
        case .success: return Adb.serverIsRunning() ? nil : "the server exits right after starting"
        case .failure(let error): return error.description
        }
    }

    private func logSelection(_ selection: HeadsetSelection) {
        let text: String
        switch selection {
        case .none: text = "no headset"
        case .ready(let device, let others): text = "\(device.displayName) \(device.serial) ready" + (others > 0 ? " (\(others) other device(s) ignored)" : "")
        case .unauthorized(let device): text = "\(device.serial) unauthorized: accept “Allow USB debugging” in the headset"
        case .authorizing(let device): text = "\(device.serial) authorizing"
        case .offline(let device): text = "\(device.serial) offline / no permissions: replug the cable"
        case .ambiguous(let count): text = "\(count) devices and none is a Quest; pick one with --adb-serial"
        }
        if text != lastLoggedSelection {
            lastLoggedSelection = text
            log("usb: \(text)")
        }
    }

    private func attach(_ headset: HeadsetAdb, model: String) {
        attachedSerial = headset.serial
        lock.lock()
        currentHeadset = headset
        lock.unlock()
        loggedExistingReverse = false
        status.installResult = nil
        // A reverse we created on another headset is gone with it.
        if let created = Self.createdReverse(), created.serial != headset.serial { Self.setCreatedReverse(nil) }
        ensureReverse(headset)
        status.app = headset.headsetApp()
        switch status.app {
        case .installed(let version): log("usb: headset app \(version) installed")
        case .notInstalled: log("usb: headset app not installed")
        case .unknown: break
        }
        if launchOnAttach { launch(headset, reason: "headset connected") }
    }

    private func detachIfNeeded() {
        guard let serial = attachedSerial else { return }
        log("usb: headset \(serial) left")
        attachedSerial = nil
        lock.lock()
        currentHeadset = nil
        lock.unlock()
        status.app = .unknown
        status.installResult = nil
        // The record of our reverse stays: if the headset only looked gone (a failed `adb
        // devices`), the reverse is still ours when it comes back, and is removed on exit.
    }

    private func ensureReverse(_ headset: HeadsetAdb) {
        let entries: [AdbReverse.Entry]
        switch headset.run(["reverse", "--list"]) {
        case .success(let output): entries = AdbReverse.parse(listOutput: output)
        case .failure(let error):
            log("usb: adb reverse --list failed: \(error)")
            return
        }
        let spec = "tcp:\(port)"
        switch AdbReverse.decide(entries, port: port) {
        case .present:
            if Self.createdReverse()?.serial != headset.serial && !loggedExistingReverse {
                loggedExistingReverse = true
                log("usb: adb reverse \(spec) \(spec) already set up (left in place on exit)")
            }
            return
        case .replace(let local):
            log("usb: replacing adb reverse \(spec) -> \(local)")
        case .create:
            break
        }
        switch headset.run(["reverse", spec, spec]) {
        case .success:
            Self.setCreatedReverse(CreatedReverse(headset: headset, port: port))
            log("usb: adb reverse \(spec) \(spec): the headset reaches this Mac at 127.0.0.1:\(port)")
        case .failure(let error):
            log("usb: adb reverse \(spec) failed: \(error)")
        }
    }

    private func launch(_ headset: HeadsetAdb, reason: String) {
        if status.app == .notInstalled {
            log("usb: not launching the headset app (\(reason)): not installed")
            return
        }
        // The app reads a non-default port from this property (empty = 7777).
        if port != 7777 {
            _ = headset.run(["shell", "setprop", "debug.vrscreen.port", "\(port)"])
        } else if case .success(let value) = headset.run(["shell", "getprop", "debug.vrscreen.port"]),
                  !value.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            _ = headset.run(["shell", "setprop", "debug.vrscreen.port", "''"])
        }
        switch headset.run(["shell", "am", "start", "-n", Self.launchActivity]) {
        case .success: log("usb: launched the headset app (\(reason))")
        case .failure(let error): log("usb: launching the headset app failed: \(error)")
        }
    }

    private func publish() {
        guard status != published else { return }
        published = status
        let snapshot = status
        DispatchQueue.main.async { self.onStatus(snapshot) }
    }

    // MARK: Cleanup of the reverse we created (also from atexit)

    private struct CreatedReverse {
        var headset: HeadsetAdb
        var port: UInt16
        var serial: String { headset.serial }
    }

    private static let createdLock = NSLock()
    private static var created: CreatedReverse?

    private static func createdReverse() -> CreatedReverse? {
        createdLock.lock()
        defer { createdLock.unlock() }
        return created
    }

    private static func setCreatedReverse(_ value: CreatedReverse?) {
        createdLock.lock()
        created = value
        createdLock.unlock()
    }

    /// Removes the reverse this process created, if any (once). Blocking, ≤ 3 s. Called on
    /// shutdown and from atexit.
    static func removeCreatedReverse() {
        createdLock.lock()
        let reverse = created
        created = nil
        createdLock.unlock()
        guard let reverse else { return }
        switch reverse.headset.run(["reverse", "--remove", "tcp:\(reverse.port)"]) {
        case .success: log("usb: removed adb reverse tcp:\(reverse.port)")
        case .failure(let error): log("usb: could not remove adb reverse tcp:\(reverse.port): \(error)")
        }
    }
}
