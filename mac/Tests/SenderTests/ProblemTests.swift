import Foundation
import MirrorProtocol
import Testing
@testable import vrscreen_sender

@Test func noProblemsByDefault() {
    #expect(ProblemList.build(HealthInputs()).isEmpty)
    // Wi-Fi only users: no adb, nothing plugged in → nothing to nag about.
    #expect(ProblemList.build(HealthInputs(usb: UsbStatus(link: .adbMissing))).isEmpty)
    #expect(ProblemList.build(HealthInputs(usb: UsbStatus(link: .noHeadset))).isEmpty)
    #expect(ProblemList.build(HealthInputs(usb: UsbStatus(link: .off))).isEmpty)
}

@Test func problemCatalogue() {
    func kinds(_ h: HealthInputs) -> [Problem.Kind] { ProblemList.build(h).map(\.kind) }
    func only(_ h: HealthInputs) -> Problem { let p = ProblemList.build(h); #expect(p.count == 1); return p[0] }

    let port = only(HealthInputs(portInUse: 7777))
    #expect(port.title == "Port 7777 in use (another VRScreen running?)" && port.actions == [.showLogs])

    // Screen Recording: only for screen capture; relaunch only for the app.
    #expect(kinds(HealthInputs(capturesScreen: false, screenRecordingGranted: false)).isEmpty)
    let tcc = only(HealthInputs(capturesScreen: true, screenRecordingGranted: false, canRelaunch: true))
    #expect(tcc.kind == .screenRecording && tcc.actions == [.openScreenRecordingSettings, .relaunch])
    #expect(only(HealthInputs(capturesScreen: true, screenRecordingGranted: false)).actions == [.openScreenRecordingSettings])
    // A permission error from ScreenCaptureKit is the same problem, not a second one.
    #expect(kinds(HealthInputs(capturesScreen: true, captureError: "declined", captureErrorIsPermission: true)) == [.screenRecording])
    #expect(kinds(HealthInputs(capturesScreen: true, screenRecordingGranted: false, captureError: "x")) == [.screenRecording])
    let capture = only(HealthInputs(capturesScreen: true, captureError: "display gone"))
    #expect(capture.kind == .captureFailed && capture.detail == "display gone" && capture.actions == [.showLogs])
    #expect(only(HealthInputs(encoderError: "VTCompressionSessionCreate failed")).kind == .encoderFailed)
    #expect(only(HealthInputs(localNetworkDenied: true)).actions == [.openLocalNetworkSettings])

    // System audio: no nag while audio is off.
    #expect(kinds(HealthInputs(audioWanted: false, tapUnavailableReason: "denied", systemAudioPermission: .denied)).isEmpty)
    let audio = only(HealthInputs(audioWanted: true, tapUnavailableReason: "denied", systemAudioPermission: .denied, canRelaunch: true))
    #expect(audio.title == "Headset audio uses the slower capture" && audio.actions == [.openSystemAudioSettings, .relaunch])
    #expect(only(HealthInputs(audioWanted: true, tapUnavailableReason: "x", systemAudioPermission: .granted, canRelaunch: true))
        .actions == [.relaunch])
    #expect(only(HealthInputs(audioWanted: true, tapUnavailableReason: "OSStatus 1", systemAudioPermission: .unknown))
        .actions == [.showLogs])

    // USB.
    let quest = "Quest 3"
    let missing = only(HealthInputs(usb: UsbStatus(link: .adbMissing, metaUSBDevice: quest)))
    #expect(missing.title == "USB needs Android platform-tools" && missing.actions == [.copyAdbInstallCommand, .openPlatformToolsPage])
    #expect(only(HealthInputs(usb: UsbStatus(link: .adbBroken("start-server: exit 1")))).actions == [.restartAdb, .showLogs])
    #expect(only(HealthInputs(usb: UsbStatus(link: .noHeadset, metaUSBDevice: quest))).title == "Quest 3 on USB, but USB debugging is off")
    #expect(only(HealthInputs(usb: UsbStatus(link: .unauthorized(serial: "U")))).title
        == "Put on the headset and accept “Allow USB debugging”")
    #expect(only(HealthInputs(usb: UsbStatus(link: .offline(serial: "O")))).kind == .usbOffline)
    #expect(only(HealthInputs(usb: UsbStatus(link: .ambiguous(count: 2)))).kind == .usbAmbiguous)
    #expect(kinds(HealthInputs(usb: UsbStatus(link: .authorizing))).isEmpty)
    let connected = UsbStatus.Link.connected(serial: "Q", model: quest, otherDevices: 0)
    let update = only(HealthInputs(usb: UsbStatus(link: connected, app: .installed(version: "0.2.0"), bundledVersion: "0.3.0")))
    #expect(update.title == "Headset app out of date (0.2.0, bundled 0.3.0)"
        && update.actions == [.installHeadsetApp(.update(from: "0.2.0", to: "0.3.0"))])
    #expect(only(HealthInputs(usb: UsbStatus(link: connected, app: .notInstalled, bundledVersion: "0.3.0"))).actions
        == [.installHeadsetApp(.install(version: "0.3.0"))])
    #expect(kinds(HealthInputs(usb: UsbStatus(link: connected, app: .installed(version: "0.3.0"), bundledVersion: "0.3.0"))).isEmpty)
    // While installing, no "install" nag; a dev binary without a bundled APK still says it is missing.
    #expect(kinds(HealthInputs(usb: UsbStatus(link: connected, app: .notInstalled, bundledVersion: "0.3.0", installing: true))).isEmpty)
    #expect(only(HealthInputs(usb: UsbStatus(link: connected, app: .notInstalled))).actions.isEmpty)
    #expect(kinds(HealthInputs(usb: UsbStatus(link: connected, serverUnstable: true, app: .installed(version: "1")))) == [.adbBroken])

    // Battery: ≤ 15 % and not on power.
    #expect(only(HealthInputs(battery: HeadsetStatus(batteryPercent: 12, charging: false, pluggedIn: false))).title
        == "Headset battery low: 12 %")
    #expect(kinds(HealthInputs(battery: HeadsetStatus(batteryPercent: 12, charging: true, pluggedIn: true))).isEmpty)
    #expect(kinds(HealthInputs(battery: HeadsetStatus(batteryPercent: 16, charging: false, pluggedIn: false))).isEmpty)
    #expect(kinds(HealthInputs(battery: HeadsetStatus(batteryPercent: nil, charging: false, pluggedIn: false))).isEmpty)
}

@Test func problemOrderErrorsFirstThenKind() {
    let h = HealthInputs(portInUse: 7777, capturesScreen: true, screenRecordingGranted: false, encoderError: "e",
                         audioWanted: true, tapUnavailableReason: "r", systemAudioPermission: .denied,
                         localNetworkDenied: true, usb: UsbStatus(link: .unauthorized(serial: "U")),
                         battery: HeadsetStatus(batteryPercent: 5, charging: false, pluggedIn: false))
    let problems = ProblemList.build(h)
    #expect(problems.map(\.kind) == [.portInUse, .screenRecording, .encoderFailed, .localNetwork,
                                     .usbUnauthorized, .systemAudio, .lowBattery])
    #expect(problems.map(\.severity) == [.error, .error, .error, .error, .warning, .warning, .warning])
    #expect(Set(problems.map(\.kind.rawValue)).count == problems.count) // one per kind
}

@Test func menuTextWrapping() {
    #expect(StatusMenu.wrap("one two three four", width: 9) == "one two\nthree\nfour")
    #expect(StatusMenu.wrap("short", width: 58) == "short")
}

@Test func logFileRotates() throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("qm-log-\(UUID().uuidString)")
    defer { try? FileManager.default.removeItem(at: dir) }
    let url = dir.appendingPathComponent("Logs/vrscreen.log")
    let file = LogFile()
    try file.open(url, maxBytes: 100)
    file.write(String(repeating: "a", count: 60) + "\n")
    #expect(!FileManager.default.fileExists(atPath: LogFile.rotatedURL(for: url).path))
    file.write(String(repeating: "b", count: 60) + "\n") // crosses 100 bytes: rotated
    file.write("c\n")
    #expect(LogFile.rotatedURL(for: url).lastPathComponent == "vrscreen.1.log")
    let old = try String(contentsOf: LogFile.rotatedURL(for: url), encoding: .utf8)
    #expect(old.hasPrefix("aaa") && old.contains("bbb"))
    #expect(try String(contentsOf: url, encoding: .utf8) == "c\n")
    // Re-opening appends and counts the existing size.
    let again = LogFile()
    try again.open(url, maxBytes: 100)
    again.write("d\n")
    #expect(try String(contentsOf: url, encoding: .utf8) == "c\nd\n")
}
