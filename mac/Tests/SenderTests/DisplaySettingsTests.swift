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
    let store = DisplaySettingsStore(url: dir.appendingPathComponent("VRScreen/settings.json"))
    #expect(store.load() == .defaults) // missing file
    try store.save(DisplaySettings(curvature: 350, dim: 1000, handGestures: true))
    #expect(store.load() == DisplaySettings(curvature: 350, dim: 1000, handGestures: true))
    // Hand-edited files: out-of-range values are clamped, missing keys default, garbage → defaults.
    try Data(#"{"curvature": 4000}"#.utf8).write(to: store.url)
    #expect(store.load() == DisplaySettings(curvature: 1000, dim: 0, handGestures: false))
    // A file from before hand gestures existed loads with them off.
    try Data(#"{"curvature": 751, "dim": 502}"#.utf8).write(to: store.url)
    #expect(store.load() == DisplaySettings(curvature: 751, dim: 502, handGestures: false))
    try Data("not json".utf8).write(to: store.url)
    #expect(store.load() == .defaults)

    #expect(DisplaySettings.defaults.handGestures == false)
    #expect(DisplaySettings(curvature: 350, dim: 20, handGestures: true).commands
        == [.setCurvature(350), .setDim(20), .setHandGestures(true)])
    #expect(DisplaySettings.permille(fromPercent: 35.04) == 350)
    #expect(DisplaySettings.permille(fromPercent: 120) == 1000)
    #expect(DisplaySettings.permille(fromPercent: -3) == 0)
}

@Test func menuStatusLine() {
    #expect(StatusMenu.statusText(forClientAddress: nil) == "No headset connected")
    #expect(StatusMenu.statusText(forClientAddress: "127.0.0.1:50123") == "Headset: USB (127.0.0.1)")
    #expect(StatusMenu.statusText(forClientAddress: "192.168.1.42:41000") == "Headset: Wi-Fi 192.168.1.42")
}
