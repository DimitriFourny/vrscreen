import Foundation
import Testing
@testable import vrscreen_sender

@Test func adbDevicesParsing() {
    let output = """
    * daemon not running; starting now at tcp:5037
    * daemon started successfully
    List of devices attached
    2G0YC5ZG8L0YQD         device usb:1-1 product:eureka model:Quest_3 device:eureka transport_id:1
    R58M12345              device usb:2-1 product:a52qnsxx model:SM_A525F device:a52q transport_id:2
    1WMHH000000000         unauthorized usb:3-1 transport_id:3
    emulator-5554          offline transport_id:4
    ABC123                 no permissions (missing udev rules? user is in the plugdev group); see [http://developer.android.com/tools/device.html] usb:1-2 transport_id:5
    XYZ                    authorizing usb:1-3 transport_id:6
    QQQ                    sideload

    """
    let devices = AdbDevice.parse(devicesOutput: output)
    #expect(devices.map(\.serial) == ["2G0YC5ZG8L0YQD", "R58M12345", "1WMHH000000000", "emulator-5554", "ABC123", "XYZ", "QQQ"])
    #expect(devices.map(\.state) == [.device, .device, .unauthorized, .offline, .noPermissions, .authorizing, .other("sideload")])
    #expect(devices[0].model == "Quest 3" && devices[0].isQuest && devices[0].product == "eureka")
    #expect(devices[1].model == "SM A525F" && !devices[1].isQuest)
    #expect(devices[2].model == nil && !devices[2].isQuest && devices[2].displayName == "Android device")
    // A Quest whose model string lacks "Quest" is recognized by its product name.
    #expect(AdbDevice(serial: "x", state: .device, model: "Meta X", product: "panther").isQuest)
    #expect(AdbDevice.parse(devicesOutput: "List of devices attached\n\n").isEmpty)
}

@Test func headsetSelection() {
    let quest = AdbDevice(serial: "Q", state: .device, model: "Quest 3", product: "eureka")
    let phone = AdbDevice(serial: "P", state: .device, model: "Pixel 8", product: "shiba")
    let unauthorized = AdbDevice(serial: "U", state: .unauthorized)
    let offline = AdbDevice(serial: "O", state: .offline)
    let authorizing = AdbDevice(serial: "A", state: .authorizing)
    #expect(HeadsetSelection.select([], preferredSerial: nil) == .none)
    #expect(HeadsetSelection.select([quest], preferredSerial: nil) == .ready(quest, others: 0))
    // Several devices: the Quest is picked by model, whatever the order.
    #expect(HeadsetSelection.select([phone, quest, unauthorized], preferredSerial: nil) == .ready(quest, others: 2))
    // A device waiting for authorization may be the headset: it beats a ready phone.
    #expect(HeadsetSelection.select([phone, unauthorized], preferredSerial: nil) == .unauthorized(unauthorized))
    #expect(HeadsetSelection.select([offline], preferredSerial: nil) == .offline(offline))
    #expect(HeadsetSelection.select([AdbDevice(serial: "N", state: .noPermissions)], preferredSerial: nil)
        == .offline(AdbDevice(serial: "N", state: .noPermissions)))
    #expect(HeadsetSelection.select([authorizing], preferredSerial: nil) == .authorizing(authorizing))
    // Only one ready device: used whatever its model; several non-Quests: ambiguous.
    #expect(HeadsetSelection.select([phone], preferredSerial: nil) == .ready(phone, others: 0))
    let phone2 = AdbDevice(serial: "P2", state: .device, model: "Pixel 7", product: "panther_x")
    #expect(HeadsetSelection.select([phone, phone2], preferredSerial: nil) == .ambiguous(count: 2))
    // --adb-serial: only that device counts.
    #expect(HeadsetSelection.select([quest, phone], preferredSerial: "P") == .ready(phone, others: 1))
    #expect(HeadsetSelection.select([quest], preferredSerial: "nope") == .none)
    // Two Quests: the lowest serial, stable across polls.
    let quest2 = AdbDevice(serial: "A2", state: .device, model: "Quest 3S", product: "panther")
    #expect(HeadsetSelection.select([quest, quest2], preferredSerial: nil) == .ready(quest2, others: 1))
}

@Test func adbReverseDecision() {
    let list = AdbReverse.parse(listOutput: "UsbFfs tcp:7777 tcp:7777\n(reverse) tcp:9000 tcp:9001\n\n")
    #expect(list == [.init(remote: "tcp:7777", local: "tcp:7777"), .init(remote: "tcp:9000", local: "tcp:9001")])
    #expect(AdbReverse.decide(list, port: 7777) == .present)
    #expect(AdbReverse.decide(list, port: 9000) == .replace(existingLocal: "tcp:9001"))
    #expect(AdbReverse.decide(list, port: 7790) == .create)
    #expect(AdbReverse.decide(AdbReverse.parse(listOutput: ""), port: 7777) == .create)
}

@Test func headsetAppVersions() {
    #expect(HeadsetAppState.parse(dumpsysVersionLines: "    versionName=0.2.0\n") == .installed(version: "0.2.0"))
    #expect(HeadsetAppState.parse(dumpsysVersionLines: "") == .notInstalled)
    #expect(compareVersions("0.2.0", "0.3.0") == .orderedAscending)
    #expect(compareVersions("0.10.0", "0.9.9") == .orderedDescending)
    #expect(compareVersions("1.0", "1.0.0") == .orderedSame)
    #expect(compareVersions("0.3.0-beta", "0.3.0") == .orderedSame)
    #expect(compareVersions("dev", "0.3.0") == nil)
    #expect(HeadsetAppAction.offer(installed: .notInstalled, bundled: "0.3.0") == .install(version: "0.3.0"))
    #expect(HeadsetAppAction.offer(installed: .installed(version: "0.2.0"), bundled: "0.3.0")
        == .update(from: "0.2.0", to: "0.3.0"))
    #expect(HeadsetAppAction.offer(installed: .installed(version: "0.3.0"), bundled: "0.3.0") == .reinstall(version: "0.3.0"))
    #expect(HeadsetAppAction.offer(installed: .installed(version: "0.4.0"), bundled: "0.3.0") == .reinstall(version: "0.3.0"))
    #expect(HeadsetAppAction.offer(installed: .installed(version: "0.2.0"), bundled: nil) == nil)
    #expect(HeadsetAppAction.offer(installed: .unknown, bundled: "0.3.0") == nil)
    #expect(HeadsetAppAction.update(from: "0.2.0", to: "0.3.0").menuTitle == "Update headset app (0.2.0 → 0.3.0)")
    #expect(adbInstallFailure("Performing Streamed Install\nSuccess\n") == nil)
    #expect(adbInstallFailure("Performing Streamed Install\nadb: failed to install x.apk: Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE: Existing package com.df0.vrscreen signatures do not match]\n")
        == "INSTALL_FAILED_UPDATE_INCOMPATIBLE")
    #expect(adbInstallFailure("Failure [INSTALL_FAILED_VERSION_DOWNGRADE]") == "INSTALL_FAILED_VERSION_DOWNGRADE")
}

@Test func usbStatusTexts() {
    var status = UsbStatus(link: .connected(serial: "2G0", model: "Quest 3", otherDevices: 1), bundledVersion: "0.3.0")
    #expect(status.linkText == "Quest 3 on USB (2G0), 1 other adb device")
    status.app = .installed(version: "0.2.0")
    #expect(status.appText == "Headset app: 0.2.0 installed · 0.3.0 bundled")
    #expect(UsbStatus(link: .noHeadset, metaUSBDevice: "Quest 3").linkText == "Quest 3 on USB without USB debugging")
    #expect(UsbStatus(link: .noHeadset).appText == nil)
}

@Test func adbRunnerTimesOutAndReportsFailures() throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("qm-adb-\(UUID().uuidString)")
    defer { try? FileManager.default.removeItem(at: dir) }
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    let script = dir.appendingPathComponent("adb")
    // A fake adb: echoes its arguments and ADB_MDNS, fails for "fail", hangs for "hang".
    try Data("""
    #!/bin/sh
    case "$1" in
      fail) echo "boom" >&2; exit 3 ;;
      hang) sleep 10 ;;
    esac
    echo "args=$* mdns=$ADB_MDNS"
    """.utf8).write(to: script)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: script.path)
    let adb = Adb(executable: script)
    #expect(try adb.run(["devices", "-l"]).get() == "args=devices -l mdns=0\n")
    #expect(adb.run(["fail"]) == .failure(.failed(status: 3, output: "boom\n")))
    let start = Date()
    #expect(adb.run(["hang"], timeout: 0.5) == .failure(.timedOut(0.5)))
    #expect(Date().timeIntervalSince(start) < 3)
}
