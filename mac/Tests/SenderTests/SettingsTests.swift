import Foundation
import MirrorProtocol
import Testing
@testable import vrscreen_sender

@Test func throttleSendsAtMostOncePerIntervalAndAlwaysTheLastValue() {
    var t = ValueThrottle<Int>(interval: 1.0 / 30)
    #expect(t.submit(1, now: 10.000) == .send(1))          // first value goes out at once
    #expect(t.submit(2, now: 10.010) == .schedule(at: 10.000 + 1.0 / 30))
    #expect(t.submit(3, now: 10.020) == .pending)          // replaces 2, timer already armed
    #expect(t.fire(now: 10.034) == 3)                      // trailing edge: the latest value
    #expect(t.fire(now: 10.040) == nil)
    #expect(t.submit(4, now: 10.050) == .schedule(at: 10.034 + 1.0 / 30))
    #expect(t.fire(now: 10.068) == 4)
    #expect(t.submit(5, now: 10.200) == .send(5))          // after a pause: immediate again

    // A 1 s drag with events every 5 ms yields ~30 sends, and ends on the final value.
    var drag = ValueThrottle<Int>(interval: 1.0 / 30)
    var sent: [Int] = []
    var timerAt: Double?
    for i in 0...200 {
        let now = Double(i) * 0.005
        if let at = timerAt, now >= at, let v = drag.fire(now: now) { sent.append(v); timerAt = nil }
        switch drag.submit(i, now: now) {
        case .send(let v): sent.append(v)
        case .schedule(let at): timerAt = at
        case .pending: break
        }
    }
    if let at = timerAt, let v = drag.fire(now: at) { sent.append(v) }
    #expect(sent.count <= 32 && sent.count >= 28)
    #expect(sent.last == 200)
}

@Test func settingsPersistAndClamp() throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("qm-settings-\(UUID().uuidString)")
    defer { try? FileManager.default.removeItem(at: dir) }
    let store = SettingsStore(url: dir.appendingPathComponent("VRScreen/settings.json"))
    #expect(store.load() == .defaults) // missing file
    try store.save(SenderSettings(curvature: 350, dim: 1000, handGestures: true))
    #expect(store.load() == SenderSettings(curvature: 350, dim: 1000, handGestures: true))
    // Hand-edited files: out-of-range values are clamped, missing keys default, garbage → defaults.
    try Data(#"{"curvature": 4000}"#.utf8).write(to: store.url)
    #expect(store.load() == SenderSettings(curvature: 1000, dim: 0, handGestures: false))
    // A file from before hand gestures existed loads with them off.
    try Data(#"{"curvature": 751, "dim": 502}"#.utf8).write(to: store.url)
    #expect(store.load() == SenderSettings(curvature: 751, dim: 502, handGestures: false))
    // A bad value only resets its own key.
    try Data(#"{"curvature": -5, "dim": 300, "codec": "vp9", "fps": 0, "bitrateMbps": "x"}"#.utf8).write(to: store.url)
    #expect(store.load() == SenderSettings(dim: 300, fps: 1))
    try Data("not json".utf8).write(to: store.url)
    #expect(store.load() == .defaults)

    #expect(SenderSettings.defaults.handGestures == false)
    #expect(SenderSettings(curvature: 350, dim: 20, handGestures: true).commands
        == [.setCurvature(350), .setDim(20), .setHandGestures(true)])
    #expect(SenderSettings.permille(fromPercent: 35.04) == 350)
    #expect(SenderSettings.permille(fromPercent: 120) == 1000)
    #expect(SenderSettings.permille(fromPercent: -3) == 0)
}

@Test func audioSettingsPersistWithDefaults() throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("qm-settings-\(UUID().uuidString)")
    defer { try? FileManager.default.removeItem(at: dir) }
    let store = SettingsStore(url: dir.appendingPathComponent("VRScreen/settings.json"))
    // Audio on and Mac mute off by default, also for files written before they existed.
    #expect(SenderSettings.defaults.audio == true)
    #expect(SenderSettings.defaults.muteMac == false)
    try FileManager.default.createDirectory(at: store.url.deletingLastPathComponent(), withIntermediateDirectories: true)
    try Data(#"{"curvature": 801, "dim": 255, "handGestures": true}"#.utf8).write(to: store.url)
    #expect(store.load() == SenderSettings(curvature: 801, dim: 255, handGestures: true, audio: true, muteMac: false))
    try store.save(SenderSettings(curvature: 10, dim: 20, handGestures: false, audio: false, muteMac: true))
    let json = try String(contentsOf: store.url, encoding: .utf8)
    #expect(json.contains(#""audio" : false"#) && json.contains(#""muteMac" : true"#))
    #expect(store.load() == SenderSettings(curvature: 10, dim: 20, handGestures: false, audio: false, muteMac: true))
    // Audio settings are Mac-side only: no CONTROL for them.
    #expect(SenderSettings(curvature: 1, dim: 2, audio: false, muteMac: true).commands
        == [.setCurvature(1), .setDim(2), .setHandGestures(false)])
}

@Test func noAudioOption() throws {
    #expect(try SenderOptions.parse([]).audio == true)
    #expect(try SenderOptions.parse(["--no-audio", "--source", "test"]).audio == false)
    #expect(try SenderOptions.parse([]).audioSource == .auto)
    #expect(try SenderOptions.parse(["--audio-source", "sck"]).audioSource == .sck)
    #expect(try SenderOptions.parse(["--audio-source", "tap"]).audioSource == .tap)
    #expect(throws: OptionError.self) { try SenderOptions.parse(["--audio-source", "mic"]) }
    #expect(try SenderOptions.parse(["--settings", "/tmp/x.json"]).settingsPath == "/tmp/x.json")
}

@Test func videoAndControlSettingsPersistWithDefaults() throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("qm-settings-\(UUID().uuidString)")
    defer { try? FileManager.default.removeItem(at: dir) }
    let store = SettingsStore(url: dir.appendingPathComponent("settings.json"))
    let d = SenderSettings.defaults
    #expect(d.maxWidth == 3840 && d.fps == 60 && d.codec == .hevc && d.bitrateMbps == 30 && d.displayID == nil)
    #expect(d.hotkeys && d.mouseGestures && d.mouseSensitivity == 0.1 && d.scrollSensitivity == 1 && d.launchHeadsetApp)
    // A file from 0.2 (headset and audio keys only) keeps its values and gets the new defaults.
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    try Data(#"{"audio": false, "curvature": 100, "dim": 0, "handGestures": false, "muteMac": true}"#.utf8).write(to: store.url)
    #expect(store.load() == SenderSettings(curvature: 100, audio: false, muteMac: true))
    var s = SenderSettings()
    s.maxWidth = 2560; s.fps = 30; s.codec = .h264; s.bitrateMbps = 15; s.displayID = 5
    s.hotkeys = false; s.mouseGestures = false; s.mouseSensitivity = 0.2; s.scrollSensitivity = 0.5; s.launchHeadsetApp = false
    try store.save(s)
    let json = try String(contentsOf: store.url, encoding: .utf8)
    #expect(json.contains(#""codec" : "h264""#) && json.contains(#""maxWidth" : 2560"#) && json.contains(#""displayID" : 5"#))
    #expect(store.load() == s)
}

@Test func commandLineOverridesApplyForTheSessionOnly() throws {
    let o = try SenderOptions.parse(["--fps", "30", "--codec", "h264", "--max-width", "1280", "--bitrate", "12.5",
                                     "--no-hotkeys", "--no-mouse", "--mouse-sensitivity", "0.3",
                                     "--scroll-sensitivity", "2", "--no-launch", "--no-adb", "--adb-serial", "X"])
    let e = o.overrides.applied(to: .defaults)
    #expect(e.fps == 30 && e.codec == .h264 && e.maxWidth == 1280 && e.bitrateMbps == 12.5)
    #expect(!e.hotkeys && !e.mouseGestures && e.mouseSensitivity == 0.3 && e.scrollSensitivity == 2 && !e.launchHeadsetApp)
    #expect(!o.adb && o.adbSerial == "X")
    #expect(try SenderOptions.parse([]).overrides.isEmpty && SenderOptions().adb)
    #expect(throws: OptionError.self) { try SenderOptions.parse(["--codec", "vp9"]) }

    // The model saves only stored values; a menu change drops that key's override.
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("qm-settings-\(UUID().uuidString)")
    defer { try? FileManager.default.removeItem(at: dir) }
    let model = SettingsModel(store: SettingsStore(url: dir.appendingPathComponent("settings.json")), overrides: o.overrides)
    var published: [SenderSettings] = []
    model.onChange = { published.append($0) }
    #expect(model.effective.fps == 30 && model.stored.fps == 60 && model.isOverridden(\.fps))
    model.set(\.fps, 60) // same as stored, but drops the override: published
    #expect(model.effective.fps == 60 && !model.isOverridden(\.fps) && published.count == 1)
    model.set(\.fps, 60) // no change, nothing published
    #expect(published.count == 1)
    model.set(\.dim, 300, publish: false)
    #expect(published.count == 1 && model.effective.dim == 300)
    #expect(model.effective.codec == .h264) // other overrides stay
    model.flushSave()
    let saved = SettingsStore(url: dir.appendingPathComponent("settings.json")).load()
    #expect(saved.fps == 60 && saved.codec == .hevc && saved.dim == 300 && saved.hotkeys)
}

@Test func videoSettingsRestartRules() {
    let base = VideoSettings(maxWidth: 3840, fps: 60, codec: .hevc, bitrateMbps: 30, display: .main)
    var v = base
    v.bitrateMbps = 50
    #expect(v.needsNewEncoder(from: base) && !v.needsNewCapture(from: base))
    v = base; v.maxWidth = 1920
    #expect(!v.needsNewEncoder(from: base) && v.needsNewCapture(from: base))
    v = base; v.fps = 30
    #expect(v.needsNewEncoder(from: base) && v.needsNewCapture(from: base))
    v = base; v.display = .id(2)
    #expect(!v.needsNewEncoder(from: base) && v.needsNewCapture(from: base))
    #expect(VideoSettings.fit(width: 5120, height: 2880, maxWidth: 3840) == (3840, 2160))
    #expect(VideoSettings.fit(width: 1920, height: 1080, maxWidth: 1280) == (1280, 720))
    #expect(VideoSettings.fit(width: 1920, height: 1080, maxWidth: 3840) == (1920, 1080)) // never scaled up
    #expect(VideoSettings.fit(width: 3456, height: 2234, maxWidth: 2560) == (2560, 1654))
}

@Test func outputPresentationLatency() {
    // Measured devices: AirPods 7680 frames, Mac mini speakers 60 + 48 + 183, a monitor 88 + 576.
    #expect(OutputLatency.micros(latencyFrames: 7680, safetyOffsetFrames: 0, streamLatencyFrames: 0, sampleRate: 48_000) == 160_000)
    #expect(OutputLatency.micros(latencyFrames: 60, safetyOffsetFrames: 48, streamLatencyFrames: 183, sampleRate: 48_000) == 6_063)
    #expect(OutputLatency.micros(latencyFrames: 88, safetyOffsetFrames: 576, streamLatencyFrames: 0, sampleRate: 44_100) == 15_057)
    #expect(OutputLatency.micros(latencyFrames: 100, safetyOffsetFrames: 0, streamLatencyFrames: 0, sampleRate: 0) == 0)
}

@Test func menuStatusLine() {
    #expect(StatusMenu.statusText(forClientAddress: nil) == "No headset connected")
    #expect(StatusMenu.statusText(forClientAddress: "127.0.0.1:50123") == "Headset: USB (127.0.0.1)")
    #expect(StatusMenu.statusText(forClientAddress: "192.168.1.42:41000") == "Headset: Wi-Fi 192.168.1.42")
}

@Test func menuBatteryLine() {
    #expect(StatusMenu.batteryText(nil) == "Battery: —")
    #expect(StatusMenu.batteryText(HeadsetStatus(batteryPercent: 72, charging: false, pluggedIn: false)) == "Battery: 72 %")
    #expect(StatusMenu.batteryText(HeadsetStatus(batteryPercent: 72, charging: true, pluggedIn: true)) == "Battery: 72 % — charging")
    #expect(StatusMenu.batteryText(HeadsetStatus(batteryPercent: 100, charging: false, pluggedIn: true))
        == "Battery: 100 % — plugged in")
    #expect(StatusMenu.batteryText(HeadsetStatus(batteryPercent: 255, flags: 0x01)) == "Battery: —")
    #expect(StatusMenu.batteryLogText(HeadsetStatus(batteryPercent: 72, charging: true, pluggedIn: false))
        == "headset battery 72 % charging")
    #expect(StatusMenu.batteryLogText(HeadsetStatus(batteryPercent: nil, charging: false, pluggedIn: false))
        == "headset battery unknown")
}
