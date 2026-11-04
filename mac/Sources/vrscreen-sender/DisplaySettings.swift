import Foundation
import MirrorProtocol

/// The settings the sender owns and keeps across restarts (`settings.json`): curvature,
/// surroundings dimming and hand gestures (the source of truth for the headset, sent to every
/// client on connect and on every change), plus the Mac-side audio switches.
struct DisplaySettings: Codable, Equatable {
    /// Permille, 0 (flat) ... 1000.
    var curvature: UInt16
    /// Permille, 0 (normal passthrough) ... 1000 (black).
    var dim: UInt16
    /// Whether the headset reacts to hand input (pinch & hold to move, two hands to resize).
    var handGestures: Bool
    /// "Audio in headset": capture the Mac's system audio and send it to the headset.
    var audio: Bool
    /// "Mute Mac while mirroring": mute the Mac's output while audio goes to a headset.
    var muteMac: Bool

    static let defaults = DisplaySettings(curvature: 0, dim: 0)

    init(curvature: UInt16, dim: UInt16, handGestures: Bool = false, audio: Bool = true, muteMac: Bool = false) {
        self.curvature = min(curvature, ControlCommand.permilleMax)
        self.dim = min(dim, ControlCommand.permilleMax)
        self.handGestures = handGestures
        self.audio = audio
        self.muteMac = muteMac
    }

    /// Out-of-range values in a hand-edited file are clamped, not rejected; missing keys
    /// (e.g. files written before a setting existed) take the defaults.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.init(curvature: try c.decodeIfPresent(UInt16.self, forKey: .curvature) ?? 0,
                  dim: try c.decodeIfPresent(UInt16.self, forKey: .dim) ?? 0,
                  handGestures: try c.decodeIfPresent(Bool.self, forKey: .handGestures) ?? false,
                  audio: try c.decodeIfPresent(Bool.self, forKey: .audio) ?? true,
                  muteMac: try c.decodeIfPresent(Bool.self, forKey: .muteMac) ?? false)
    }

    /// CONTROL messages that put a client in this state.
    var commands: [ControlCommand] { [.setCurvature(curvature), .setDim(dim), .setHandGestures(handGestures)] }

    /// Slider percentage (0...100) → permille.
    static func permille(fromPercent percent: Double) -> UInt16 {
        UInt16((min(max(percent, 0), 100) * 10).rounded())
    }
}

/// JSON file store: `~/Library/Application Support/VRScreen/settings.json`.
/// A missing or unreadable file yields the defaults.
struct DisplaySettingsStore {
    let url: URL

    static var standard: DisplaySettingsStore {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return DisplaySettingsStore(url: base.appendingPathComponent("VRScreen/settings.json"))
    }

    func load() -> DisplaySettings {
        guard let data = try? Data(contentsOf: url),
              let settings = try? JSONDecoder().decode(DisplaySettings.self, from: data) else { return .defaults }
        return settings
    }

    func save(_ settings: DisplaySettings) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(settings).write(to: url, options: .atomic)
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
