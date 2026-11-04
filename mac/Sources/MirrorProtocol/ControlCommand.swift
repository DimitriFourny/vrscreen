import Foundation

/// Payload of `0x03 CONTROL` (server → client): `command u8 | args`. Asks the client to
/// move the virtual screen. Step sizes are client-defined (docs/protocol.md).
/// Multi-byte arguments are big-endian. Command `0x02` is reserved (formerly MOVE).
public enum ControlCommand: Equatable, Sendable, CustomStringConvertible {
    /// Place the screen in front of the current head direction (keep distance/size).
    case recenter
    /// `steps` > 0 = farther.
    case distance(steps: Int8)
    /// `steps` > 0 = bigger.
    case size(steps: Int8)
    /// Default distance and size, then recenter.
    case reset
    /// Slide the screen in its own plane: `dx` steps right, `dy` steps up.
    case translate(dx: Int8, dy: Int8)
    /// Rotate the screen around the head, in 1/100 degree: `yaw` + = right, `pitch` + = up.
    case orbit(yawCentidegrees: Int16, pitchCentidegrees: Int16)
    /// Distance × 1.1^(value/100); + = farther (100 = one DISTANCE step).
    case distanceFine(Int16)
    /// Width × 1.1^(value/100); + = bigger (100 = one SIZE step).
    case sizeFine(Int16)
    /// Absolute screen curvature in permille (0 = flat, 1000 = cylinder radius equal to
    /// the viewing distance; radius = distance / (value/1000)). Receivers clamp to 1000.
    case setCurvature(UInt16)
    /// Absolute dimming of the surroundings (passthrough) in permille: 0 = normal,
    /// 1000 = black. Never dims the screen. Receivers clamp to 1000.
    case setDim(UInt16)
    /// A command this implementation does not know; receivers ignore it.
    case unknown(command: UInt8, args: Data)

    public static let recenterCode: UInt8 = 0x01
    public static let distanceCode: UInt8 = 0x03
    public static let sizeCode: UInt8 = 0x04
    public static let resetCode: UInt8 = 0x05
    public static let translateCode: UInt8 = 0x06
    public static let orbitCode: UInt8 = 0x07
    public static let distanceFineCode: UInt8 = 0x08
    public static let sizeFineCode: UInt8 = 0x09
    public static let setCurvatureCode: UInt8 = 0x0A
    public static let setDimCode: UInt8 = 0x0B
    /// Upper end of SET_CURVATURE / SET_DIM values.
    public static let permilleMax: UInt16 = 1000

    /// The CONTROL payload.
    public func encoded() -> Data {
        var data = Data()
        switch self {
        case .recenter: data.append(Self.recenterCode)
        case .distance(let steps): data.append(contentsOf: [Self.distanceCode, UInt8(bitPattern: steps)])
        case .size(let steps): data.append(contentsOf: [Self.sizeCode, UInt8(bitPattern: steps)])
        case .reset: data.append(Self.resetCode)
        case .translate(let dx, let dy):
            data.append(contentsOf: [Self.translateCode, UInt8(bitPattern: dx), UInt8(bitPattern: dy)])
        case .orbit(let yaw, let pitch):
            data.append(Self.orbitCode)
            data.appendBigEndian(yaw)
            data.appendBigEndian(pitch)
        case .distanceFine(let v):
            data.append(Self.distanceFineCode)
            data.appendBigEndian(v)
        case .sizeFine(let v):
            data.append(Self.sizeFineCode)
            data.appendBigEndian(v)
        case .setCurvature(let v):
            data.append(Self.setCurvatureCode)
            data.appendBigEndian(v)
        case .setDim(let v):
            data.append(Self.setDimCode)
            data.appendBigEndian(v)
        case .unknown(let command, let args):
            data.append(command)
            data.append(args)
        }
        return data
    }

    /// Parses a CONTROL payload. Unknown commands decode as `.unknown`; bytes after
    /// a known command's arguments are ignored.
    public static func decode(_ payload: Data) throws(ProtocolError) -> ControlCommand {
        var r = ByteReader(payload)
        guard let command = r.u8() else {
            throw .malformed(type: MessageType.control.rawValue, reason: "CONTROL payload is empty")
        }
        let missing = ProtocolError.malformed(type: MessageType.control.rawValue,
                                              reason: "CONTROL command \(command) is missing arguments")
        func i8() throws(ProtocolError) -> Int8 {
            guard let v = r.u8() else { throw missing }
            return Int8(bitPattern: v)
        }
        func i16() throws(ProtocolError) -> Int16 {
            guard let v = r.u16() else { throw missing }
            return Int16(bitPattern: v)
        }
        switch command {
        case recenterCode: return .recenter
        case distanceCode: return .distance(steps: try i8())
        case sizeCode: return .size(steps: try i8())
        case resetCode: return .reset
        case translateCode: return .translate(dx: try i8(), dy: try i8())
        case orbitCode: return .orbit(yawCentidegrees: try i16(), pitchCentidegrees: try i16())
        case distanceFineCode: return .distanceFine(try i16())
        case sizeFineCode: return .sizeFine(try i16())
        case setCurvatureCode, setDimCode:
            guard let v = r.u16() else { throw missing }
            return command == setCurvatureCode ? .setCurvature(v) : .setDim(v)
        default: return .unknown(command: command, args: r.rest())
        }
    }

    public var description: String {
        func signed<T: SignedInteger>(_ v: T) -> String { v > 0 ? "+\(v)" : "\(v)" }
        func degrees(_ v: Int16) -> String { String(format: "%+.2f°", Double(v) / 100) }
        switch self {
        case .recenter: return "RECENTER"
        case .distance(let steps): return "DISTANCE \(signed(steps))"
        case .size(let steps): return "SIZE \(signed(steps))"
        case .reset: return "RESET"
        case .translate(let dx, let dy): return "TRANSLATE dx=\(signed(dx)) dy=\(signed(dy))"
        case .orbit(let yaw, let pitch): return "ORBIT yaw=\(degrees(yaw)) pitch=\(degrees(pitch))"
        case .distanceFine(let v): return "DISTANCE_FINE \(signed(v))"
        case .sizeFine(let v): return "SIZE_FINE \(signed(v))"
        case .setCurvature(let v): return "SET_CURVATURE \(v) (\(String(format: "%.1f", Double(v) / 10)) %)"
        case .setDim(let v): return "SET_DIM \(v) (\(String(format: "%.1f", Double(v) / 10)) %)"
        case .unknown(let command, let args): return "unknown command 0x\(String(command, radix: 16)) (\(args.count) arg bytes)"
        }
    }
}
