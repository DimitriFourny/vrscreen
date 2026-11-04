import Foundation
import Testing
@testable import quest_mirror_sender

@Test func travelModeParsing() {
    // Excerpt of `dumpsys tracking` on Horizon OS 209: the unrelated "TravelMode: off"
    // line must not be mistaken for the state.
    let dump = """
    Tracking state
        TravelMode: off
     * Travel Mode: true
    * TravelModeClassifierRunner *
    """
    #expect(TravelModeState.parse(dumpsysTracking: dump) == .on)
    #expect(TravelModeState.parse(dumpsysTracking: " * Travel Mode: false\r\n") == .off)
    #expect(TravelModeState.parse(dumpsysTracking: "* Travel Mode:   TRUE") == .on)
    // Missing line, other spelling only, or an unexpected value → format changed.
    #expect(TravelModeState.parse(dumpsysTracking: "") == .formatChanged)
    #expect(TravelModeState.parse(dumpsysTracking: "    TravelMode: off\n") == .formatChanged)
    #expect(TravelModeState.parse(dumpsysTracking: " * Travel Mode: enabled") == .formatChanged)
    #expect(TravelModeState.parse(dumpsysTracking: "\u{0}\u{FFFD}garbage * Travel Mod") == .formatChanged)
}

@Test func travelModeMenuState() {
    #expect(TravelModeState.on.menuTitle == "Travel mode: On")
    #expect(TravelModeState.notOnUSB.menuTitle == "Travel mode: unknown — headset not on USB")
    #expect(TravelModeState.on.headsetReachable && TravelModeState.formatChanged.headsetReachable)
    #expect(!TravelModeState.notOnUSB.headsetReachable && !TravelModeState.adbMissing.headsetReachable
        && !TravelModeState.adbServerDown.headsetReachable)
}

@Test func adbLookupPrefersPATH() throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("qm-adb-\(UUID().uuidString)")
    defer { try? FileManager.default.removeItem(at: dir) }
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    let fake = dir.appendingPathComponent("adb")
    try Data("#!/bin/sh\n".utf8).write(to: fake)
    try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: fake.path)
    #expect(HeadsetAdb.locateExecutable(environment: ["PATH": "/nonexistent:\(dir.path)", "HOME": "/nonexistent"]) == fake)
    // Not executable / not present → falls through to the fixed locations (absent here).
    let empty = dir.appendingPathComponent("empty")
    try FileManager.default.createDirectory(at: empty, withIntermediateDirectories: true)
    let found = HeadsetAdb.locateExecutable(environment: ["PATH": empty.path, "HOME": "/nonexistent"])
    #expect(found == nil || found!.path.hasPrefix("/opt/homebrew") || found!.path.hasPrefix("/usr/local"))
}
