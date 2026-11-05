import Foundation
import MirrorProtocol

/// Everything the user can change in the menu, kept across restarts in `settings.json`.
/// Curvature, dimming and hand gestures are the source of truth for the headset (sent to
/// every client on connect and on every change); the rest is Mac-side.
struct SenderSettings: Equatable {
    enum Codec: String, CaseIterable {
        case hevc, h264

        var videoCodec: VideoCodec { self == .hevc ? .hevc : .h264 }
    }

    // Headset display (CONTROL messages).
    /// Permille, 0 (flat) ... 1000.
    var curvature: UInt16 = 0
    /// Permille, 0 (normal passthrough) ... 1000 (black).
    var dim: UInt16 = 0
    /// Whether the headset reacts to hand input (pinch & hold to move, two hands to resize).
    var handGestures = false

    // Audio.
    /// "Audio in headset": capture the Mac's system audio and send it to the headset.
    var audio = true
    /// "Mute Mac while mirroring": mute the Mac's output while audio goes to a headset.
    var muteMac = false

    // Video (changes restart capture and/or the encoder).
    /// The display's native pixel size is scaled down to fit this width.
    var maxWidth = 3840
    var fps = 60
    var codec = Codec.hevc
    /// Average bitrate, Mbit/s.
    var bitrateMbps = 30.0
    /// CGDirectDisplayID of the captured display; nil = the main display.
    var displayID: UInt32?

    // Controls.
    /// The global ⌃⌥⌘ shortcuts.
    var hotkeys = true
    /// ⌃⌥⌘ + mouse (orbit) and ⌃⌥⌘ + scroll (distance / size).
    var mouseGestures = true
    /// Orbit degrees per point of mouse movement.
    var mouseSensitivity = 0.1
    /// Multiplier on the scroll → DISTANCE_FINE / SIZE_FINE scaling.
    var scrollSensitivity = 1.0

    // USB.
    /// "Open headset app when connected": start the app when the headset appears on USB.
    var launchHeadsetApp = true

    static let defaults = SenderSettings()

    /// Menu presets (the command line can set other values).
    static let maxWidthPresets = [3840, 2560, 1920]
    static let fpsPresets = [60, 30]
    static let bitratePresets = [15.0, 30.0, 50.0]
    static let mouseSensitivityPresets: [(label: String, value: Double)] = [("Low", 0.05), ("Normal", 0.1), ("High", 0.2)]
    static let scrollSensitivityPresets: [(label: String, value: Double)] = [("Low", 0.5), ("Normal", 1.0), ("High", 2.0)]

    /// CONTROL messages that put a client in this state.
    var commands: [ControlCommand] { [.setCurvature(curvature), .setDim(dim), .setHandGestures(handGestures)] }

    /// Slider percentage (0...100) → permille.
    static func permille(fromPercent percent: Double) -> UInt16 {
        UInt16((min(max(percent, 0), 100) * 10).rounded())
    }

    /// One line for the log.
    var summary: String {
        "curvature \(curvature / 10) %, dim \(dim / 10) %, hand gestures \(onOff(handGestures)), "
            + "audio \(onOff(audio)), mute Mac \(onOff(muteMac)), video \(videoSummary), "
            + "display \(displayID.map(String.init) ?? "main"), shortcuts \(onOff(hotkeys)), "
            + "mouse \(onOff(mouseGestures)) (\(mouseSensitivity)°/pt, scroll ×\(scrollSensitivity)), "
            + "open headset app \(onOff(launchHeadsetApp))"
    }

    var videoSummary: String {
        "max \(maxWidth) px, \(fps) fps, \(codec.videoCodec), \(Self.format(mbps: bitrateMbps)) Mbit/s"
    }

    static func format(mbps: Double) -> String {
        mbps == mbps.rounded() ? String(Int(mbps)) : String(format: "%.1f", mbps)
    }

    private func onOff(_ value: Bool) -> String { value ? "on" : "off" }
}

extension SenderSettings: Codable {
    enum CodingKeys: String, CodingKey {
        case curvature, dim, handGestures, audio, muteMac, maxWidth, fps, codec, bitrateMbps, displayID
        case hotkeys, mouseGestures, mouseSensitivity, scrollSensitivity, launchHeadsetApp
    }

    /// Hand-edited files: out-of-range values are clamped, a key with a wrong type or value
    /// and missing keys (files written before a setting existed) take the default.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        func value<T: Decodable>(_ key: CodingKeys, _ fallback: T) -> T {
            (try? c.decodeIfPresent(T.self, forKey: key)) ?? fallback
        }
        let d = SenderSettings.defaults
        curvature = min(value(.curvature, d.curvature), ControlCommand.permilleMax)
        dim = min(value(.dim, d.dim), ControlCommand.permilleMax)
        handGestures = value(.handGestures, d.handGestures)
        audio = value(.audio, d.audio)
        muteMac = value(.muteMac, d.muteMac)
        maxWidth = min(max(value(.maxWidth, d.maxWidth), 16), 16_384)
        fps = min(max(value(.fps, d.fps), 1), 240)
        codec = Codec(rawValue: value(.codec, d.codec.rawValue).lowercased()) ?? d.codec
        let bitrate = value(.bitrateMbps, d.bitrateMbps)
        bitrateMbps = bitrate.isFinite ? min(max(bitrate, 0.5), 500) : d.bitrateMbps
        displayID = (try? c.decodeIfPresent(UInt32.self, forKey: .displayID)) ?? nil
        hotkeys = value(.hotkeys, d.hotkeys)
        mouseGestures = value(.mouseGestures, d.mouseGestures)
        let mouse = value(.mouseSensitivity, d.mouseSensitivity)
        mouseSensitivity = mouse.isFinite ? min(max(mouse, 0.001), 10) : d.mouseSensitivity
        let scroll = value(.scrollSensitivity, d.scrollSensitivity)
        scrollSensitivity = scroll.isFinite ? min(max(scroll, 0.01), 100) : d.scrollSensitivity
        launchHeadsetApp = value(.launchHeadsetApp, d.launchHeadsetApp)
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(curvature, forKey: .curvature)
        try c.encode(dim, forKey: .dim)
        try c.encode(handGestures, forKey: .handGestures)
        try c.encode(audio, forKey: .audio)
        try c.encode(muteMac, forKey: .muteMac)
        try c.encode(maxWidth, forKey: .maxWidth)
        try c.encode(fps, forKey: .fps)
        try c.encode(codec.rawValue, forKey: .codec)
        try c.encode(bitrateMbps, forKey: .bitrateMbps)
        try c.encodeIfPresent(displayID, forKey: .displayID)
        try c.encode(hotkeys, forKey: .hotkeys)
        try c.encode(mouseGestures, forKey: .mouseGestures)
        try c.encode(mouseSensitivity, forKey: .mouseSensitivity)
        try c.encode(scrollSensitivity, forKey: .scrollSensitivity)
        try c.encode(launchHeadsetApp, forKey: .launchHeadsetApp)
    }
}

/// Session-only values from command-line flags, applied on top of the stored settings.
/// They are never saved; changing the same setting in the menu drops the override.
struct SettingsOverrides {
    private var entries: [(key: PartialKeyPath<SenderSettings>, apply: (inout SenderSettings) -> Void)] = []

    var isEmpty: Bool { entries.isEmpty }

    mutating func set<T>(_ key: WritableKeyPath<SenderSettings, T>, _ value: T) {
        remove(key)
        entries.append((key, { $0[keyPath: key] = value }))
    }

    mutating func remove(_ key: PartialKeyPath<SenderSettings>) {
        entries.removeAll { $0.key == key }
    }

    func contains(_ key: PartialKeyPath<SenderSettings>) -> Bool {
        entries.contains { $0.key == key }
    }

    func applied(to settings: SenderSettings) -> SenderSettings {
        var result = settings
        for entry in entries { entry.apply(&result) }
        return result
    }
}

/// JSON file store: `~/Library/Application Support/VRScreen/settings.json`.
/// A missing or unreadable file yields the defaults.
struct SettingsStore {
    let url: URL

    static var standard: SettingsStore {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return SettingsStore(url: base.appendingPathComponent("VRScreen/settings.json"))
    }

    func load() -> SenderSettings {
        guard let data = try? Data(contentsOf: url),
              let settings = try? JSONDecoder().decode(SenderSettings.self, from: data) else { return .defaults }
        return settings
    }

    func save(_ settings: SenderSettings) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(settings).write(to: url, options: .atomic)
    }
}

/// The stored settings plus the session overrides, and the debounced save. Main thread only.
final class SettingsModel {
    /// Settings are written to disk this long after the last change.
    static let saveDelay: TimeInterval = 0.5

    let store: SettingsStore
    private(set) var stored: SenderSettings
    private var overrides: SettingsOverrides
    private var pendingSave: DispatchWorkItem?
    /// Called with the effective settings after every change that is published.
    var onChange: ((SenderSettings) -> Void)?

    init(store: SettingsStore, overrides: SettingsOverrides) {
        self.store = store
        self.overrides = overrides
        stored = store.load()
    }

    /// Stored settings with the command-line overrides applied: what the sender uses.
    var effective: SenderSettings { overrides.applied(to: stored) }

    func isOverridden(_ key: PartialKeyPath<SenderSettings>) -> Bool { overrides.contains(key) }

    /// Changes one setting (dropping its command-line override), schedules the save and,
    /// unless `publish` is false (slider drags are throttled by the caller), notifies.
    func set<T: Equatable>(_ key: WritableKeyPath<SenderSettings, T>, _ value: T, publish: Bool = true) {
        let wasOverridden = overrides.contains(key)
        guard stored[keyPath: key] != value || wasOverridden else { return }
        overrides.remove(key)
        stored[keyPath: key] = value
        scheduleSave()
        if publish { self.publish() }
    }

    /// Sends the current effective settings to `onChange`.
    func publish() {
        onChange?(effective)
    }

    /// Re-reads the file (after a hand edit; SIGUSR1). Overrides stay.
    func reload() {
        stored = store.load()
        log("settings: reloaded \(store.url.path): \(effective.summary)")
        publish()
    }

    /// Writes a pending change now (before quitting).
    func flushSave() {
        guard let pendingSave else { return }
        pendingSave.cancel()
        self.pendingSave = nil
        save()
    }

    private func scheduleSave() {
        pendingSave?.cancel()
        let work = DispatchWorkItem { [weak self] in
            self?.pendingSave = nil
            self?.save()
        }
        pendingSave = work
        DispatchQueue.main.asyncAfter(deadline: .now() + Self.saveDelay, execute: work)
    }

    private func save() {
        do {
            try store.save(stored)
            log("settings: \(stored.summary) (saved)")
        } catch {
            log("settings: could not save to \(store.url.path): \(error)")
        }
    }
}

/// Rate limit for slider drags: at most one value per `interval`, and the last value
/// submitted is always delivered (trailing edge). Pure: the caller supplies time and
/// arms a timer when told to.
struct ValueThrottle<Value: Equatable> {
    enum Action: Equatable {
        /// Send `Value` now.
        case send(Value)
        /// Nothing to send yet; call `fire(now:)` at this time (only returned once per wait).
        case schedule(at: TimeInterval)
        /// A trailing send is already scheduled; the new value replaced the pending one.
        case pending
    }

    let interval: TimeInterval
    private var lastSent: TimeInterval?
    private var pendingValue: Value?
    private var timerArmed = false

    init(interval: TimeInterval) {
        self.interval = interval
    }

    mutating func submit(_ value: Value, now: TimeInterval) -> Action {
        if let lastSent, now - lastSent < interval {
            pendingValue = value
            if timerArmed { return .pending }
            timerArmed = true
            return .schedule(at: lastSent + interval)
        }
        lastSent = now
        pendingValue = nil
        return .send(value)
    }

    /// The scheduled time came: the value to send, if one is still pending.
    mutating func fire(now: TimeInterval) -> Value? {
        timerArmed = false
        guard let value = pendingValue else { return nil }
        pendingValue = nil
        lastSent = now
        return value
    }
}
