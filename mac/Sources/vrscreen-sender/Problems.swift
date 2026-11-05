import Foundation
import MirrorProtocol

/// Everything the problem list is computed from. Gathered by `main` from events (capture,
/// encoder, server, discovery, USB, headset status) and cheap checks re-run when the menu
/// opens (Screen Recording / System Audio Recording preflights).
struct HealthInputs: Equatable {
    /// The TCP or discovery port that another process holds.
    var portInUse: UInt16?
    /// Screen capture (not the test pattern) is in use.
    var capturesScreen = false
    /// CGPreflightScreenCaptureAccess().
    var screenRecordingGranted = true
    /// The last capture start failed (cleared when capture runs).
    var captureError: String?
    /// The capture failure is ScreenCaptureKit's "permission denied".
    var captureErrorIsPermission = false
    var encoderError: String?
    /// Audio is on in the settings and not disabled by --no-audio.
    var audioWanted = false
    /// Why the process tap is not used although it was wanted (ScreenCaptureKit audio instead).
    var tapUnavailableReason: String?
    var systemAudioPermission = SystemAudioPermission.unknown
    /// A LAN send failed with "no route to host" (macOS 15+ Local Network privacy).
    var localNetworkDenied = false
    /// nil with --no-adb.
    var usb: UsbStatus?
    var battery: HeadsetStatus?
    /// Running as VRScreen.app (can relaunch itself).
    var canRelaunch = false
}

struct Problem: Equatable {
    /// Also the display order within a severity.
    enum Kind: Int, Comparable {
        case portInUse, screenRecording, captureFailed, encoderFailed, localNetwork
        case usbDebuggingOff, adbMissing, adbBroken, usbUnauthorized, usbOffline, usbAmbiguous, headsetApp
        case systemAudio, lowBattery

        static func < (a: Kind, b: Kind) -> Bool { a.rawValue < b.rawValue }
    }

    enum Severity: Int, Comparable {
        case error, warning

        static func < (a: Severity, b: Severity) -> Bool { a.rawValue < b.rawValue }
    }

    enum Action: Equatable {
        case openScreenRecordingSettings
        case openSystemAudioSettings
        case openLocalNetworkSettings
        case relaunch
        case showLogs
        case copyAdbInstallCommand
        case openPlatformToolsPage
        case restartAdb
        case installHeadsetApp(HeadsetAppAction)

        var title: String {
            switch self {
            case .openScreenRecordingSettings: "Grant Screen Recording…"
            case .openSystemAudioSettings: "Grant System Audio Recording…"
            case .openLocalNetworkSettings: "Allow Local Network Access…"
            case .relaunch: "Relaunch VRScreen"
            case .showLogs: "Show Logs…"
            case .copyAdbInstallCommand: "Copy Install Command (\(Adb.installCommand))"
            case .openPlatformToolsPage: "Download Platform-Tools…"
            case .restartAdb: "Restart adb"
            case .installHeadsetApp(let action): action.menuTitle
            }
        }
    }

    var kind: Kind
    var severity: Severity
    var title: String
    var detail: String?
    var actions: [Action] = []
}

enum ProblemList {
    static let lowBatteryPercent = 15

    /// The problems to show, errors first, one per kind, in `Kind` order.
    static func build(_ h: HealthInputs) -> [Problem] {
        var problems: [Problem] = []
        let relaunch: [Problem.Action] = h.canRelaunch ? [.relaunch] : []

        if let port = h.portInUse {
            problems.append(Problem(kind: .portInUse, severity: .error,
                                    title: "Port \(port) in use (another VRScreen running?)",
                                    detail: "Quit the other VRScreen or dev sender; this one keeps retrying.",
                                    actions: [.showLogs]))
        }
        let screenRecordingMissing = h.capturesScreen && (!h.screenRecordingGranted || h.captureErrorIsPermission)
        if screenRecordingMissing {
            problems.append(Problem(kind: .screenRecording, severity: .error,
                                    title: "Screen Recording not allowed",
                                    detail: "Allow VRScreen in Privacy & Security › Screen & System Audio Recording. "
                                        + "macOS may require relaunching VRScreen afterwards.",
                                    actions: [.openScreenRecordingSettings] + relaunch))
        } else if let error = h.captureError {
            problems.append(Problem(kind: .captureFailed, severity: .error,
                                    title: "Screen capture failed", detail: error, actions: [.showLogs]))
        }
        if let error = h.encoderError {
            problems.append(Problem(kind: .encoderFailed, severity: .error,
                                    title: "Video encoder error", detail: error, actions: [.showLogs]))
        }
        if h.localNetworkDenied {
            problems.append(Problem(kind: .localNetwork, severity: .error,
                                    title: "Local Network access denied",
                                    detail: "The headset can't reach this Mac over Wi-Fi (USB still works). "
                                        + "Allow VRScreen in Privacy & Security › Local Network.",
                                    actions: [.openLocalNetworkSettings] + relaunch))
        }
        if let usb = h.usb { problems += usbProblems(usb) }
        if h.audioWanted, let reason = h.tapUnavailableReason {
            switch h.systemAudioPermission {
            case .denied:
                problems.append(Problem(kind: .systemAudio, severity: .warning,
                                        title: "Headset audio uses the slower capture",
                                        detail: "Allow VRScreen under System Audio Recording Only (Privacy & Security › "
                                            + "Screen & System Audio Recording), then relaunch: sound arrives ~30 ms sooner.",
                                        actions: [.openSystemAudioSettings] + relaunch))
            case .granted:
                problems.append(Problem(kind: .systemAudio, severity: .warning,
                                        title: "System Audio Recording allowed: relaunch to use it",
                                        detail: "Headset audio uses the slower capture until VRScreen restarts.",
                                        actions: relaunch.isEmpty ? [.showLogs] : relaunch))
            case .unknown:
                problems.append(Problem(kind: .systemAudio, severity: .warning,
                                        title: "Headset audio uses the slower capture",
                                        detail: "Process tap unavailable: \(reason)", actions: [.showLogs]))
            }
        }
        if let battery = h.battery, let percent = battery.battery, percent <= lowBatteryPercent,
           !battery.isCharging, !battery.isPluggedIn {
            problems.append(Problem(kind: .lowBattery, severity: .warning, title: "Headset battery low: \(percent) %"))
        }
        // One per kind (the builders above never add two, this keeps it true), errors first.
        var seen = Set<Int>()
        return problems.filter { seen.insert($0.kind.rawValue).inserted }
            .sorted { ($0.severity, $0.kind) < ($1.severity, $1.kind) }
    }

    private static func usbProblems(_ usb: UsbStatus) -> [Problem] {
        switch usb.link {
        case .off, .authorizing:
            return []
        case .adbMissing:
            // Only worth a warning when a headset is actually plugged in; otherwise the
            // Headset submenu explains it and Wi-Fi is fine.
            guard let device = usb.metaUSBDevice else { return [] }
            return [Problem(kind: .adbMissing, severity: .warning, title: "USB needs Android platform-tools",
                            detail: "\(device) is plugged in, but adb is not installed. Wi-Fi works without it.",
                            actions: [.copyAdbInstallCommand, .openPlatformToolsPage])]
        case .adbBroken(let reason):
            return [Problem(kind: .adbBroken, severity: .warning, title: "adb isn't working",
                            detail: reason, actions: [.restartAdb, .showLogs])]
        case .noHeadset:
            guard let device = usb.metaUSBDevice else { return [] }
            return [Problem(kind: .usbDebuggingOff, severity: .warning,
                            title: "\(device) on USB, but USB debugging is off",
                            detail: "Turn on Developer Mode (Meta Horizon phone app › Devices › Headset settings), "
                                + "restart the headset, replug the cable and accept “Allow USB debugging”.")]
        case .unauthorized:
            return [Problem(kind: .usbUnauthorized, severity: .warning,
                            title: "Put on the headset and accept “Allow USB debugging”",
                            detail: "Tick “Always allow from this computer”.")]
        case .offline:
            return [Problem(kind: .usbOffline, severity: .warning, title: "Headset offline on USB",
                            detail: "Unplug and replug the cable (use a data cable).")]
        case .ambiguous(let count):
            return [Problem(kind: .usbAmbiguous, severity: .warning, title: "\(count) adb devices, none is a Quest",
                            detail: "Unplug the other devices, or start the sender with --adb-serial.")]
        case .connected:
            var problems: [Problem] = []
            if usb.serverUnstable {
                problems.append(Problem(kind: .adbBroken, severity: .warning, title: "adb isn't working",
                                        detail: "The adb server keeps stopping.", actions: [.restartAdb, .showLogs]))
            }
            guard !usb.installing else { return problems }
            if let offer = HeadsetAppAction.offer(installed: usb.app, bundled: usb.bundledVersion), offer.isNeeded {
                let title = switch offer {
                case .update(let from, let to): "Headset app out of date (\(from), bundled \(to))"
                default: "Headset app not installed"
                }
                problems.append(Problem(kind: .headsetApp, severity: .warning, title: title,
                                        actions: [.installHeadsetApp(offer)]))
            } else if usb.bundledVersion == nil, usb.app == .notInstalled {
                problems.append(Problem(kind: .headsetApp, severity: .warning, title: "Headset app not installed",
                                        detail: "Install it with scripts/mirror.sh --install (or use VRScreen.app)."))
            }
            return problems
        }
    }
}
