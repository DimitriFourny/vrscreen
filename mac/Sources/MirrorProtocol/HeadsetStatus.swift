/// `0x13 HEADSET_STATUS` (client → server): `batteryPercent u8 | flags u8`.
public struct HeadsetStatus: Equatable, Sendable {
    /// `batteryPercent` value when the client does not know the level.
    public static let unknownBattery: UInt8 = 255
    public static let chargingFlag: UInt8 = 0x01
    public static let pluggedInFlag: UInt8 = 0x02

    /// 0-100, or `unknownBattery` (other values above 100 are treated as unknown).
    public var batteryPercent: UInt8
    /// Bit 0 charging, bit 1 plugged in; other bits reserved (ignored).
    public var flags: UInt8

    public init(batteryPercent: UInt8, flags: UInt8) {
        self.batteryPercent = batteryPercent
        self.flags = flags
    }

    public init(batteryPercent: UInt8?, charging: Bool, pluggedIn: Bool) {
        self.batteryPercent = batteryPercent ?? Self.unknownBattery
        flags = (charging ? Self.chargingFlag : 0) | (pluggedIn ? Self.pluggedInFlag : 0)
    }

    /// The level, or nil when unknown.
    public var battery: UInt8? { batteryPercent <= 100 ? batteryPercent : nil }
    public var isCharging: Bool { flags & Self.chargingFlag != 0 }
    public var isPluggedIn: Bool { flags & Self.pluggedInFlag != 0 }
}
