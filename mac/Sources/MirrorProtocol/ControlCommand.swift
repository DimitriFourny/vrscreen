import Foundation

/// Payload of `0x03 CONTROL` (server → client): `command u8 | args`. Asks the client to
/// move the virtual screen. Step sizes are client-defined (docs/protocol.md).
public enum ControlCommand: Equatable, Sendable, CustomStringConvertible {
    /// Place the screen in front of the current head direction (keep distance/size).
    case recenter
    /// Rotate the screen around the user: `dx` yaw steps (+ = right), `dy` pitch steps (+ = up).
    case move(dx: Int8, dy: Int8)
    /// `steps` > 0 = farther.
    case distance(steps: Int8)
    /// `steps` > 0 = bigger.
    case size(steps: Int8)
    /// Default distance and size, then recenter.
    case reset
    /// A command this implementation does not know; receivers ignore it.
    case unknown(command: UInt8, args: Data)

    public static let recenterCode: UInt8 = 0x01
    public static let moveCode: UInt8 = 0x02
    public static let distanceCode: UInt8 = 0x03
    public static let sizeCode: UInt8 = 0x04
    public static let resetCode: UInt8 = 0x05

    /// The CONTROL payload.
    public func encoded() -> Data {
        switch self {
        case .recenter: Data([Self.recenterCode])
        case .move(let dx, let dy): Data([Self.moveCode, UInt8(bitPattern: dx), UInt8(bitPattern: dy)])
        case .distance(let steps): Data([Self.distanceCode, UInt8(bitPattern: steps)])
        case .size(let steps): Data([Self.sizeCode, UInt8(bitPattern: steps)])
        case .reset: Data([Self.resetCode])
        case .unknown(let command, let args): Data([command]) + args
        }
    }

    /// Parses a CONTROL payload. Unknown commands decode as `.unknown`; bytes after
    /// a known command's arguments are ignored.
    public static func decode(_ payload: Data) throws(ProtocolError) -> ControlCommand {
        let bytes = [UInt8](payload)
        guard let command = bytes.first else {
            throw .malformed(type: MessageType.control.rawValue, reason: "CONTROL payload is empty")
        }
        func arg(_ i: Int) throws(ProtocolError) -> Int8 {
            guard bytes.count > i else {
                throw .malformed(type: MessageType.control.rawValue, reason: "CONTROL command \(command) is missing arguments")
            }
            return Int8(bitPattern: bytes[i])
        }
        switch command {
        case recenterCode: return .recenter
        case moveCode: return .move(dx: try arg(1), dy: try arg(2))
        case distanceCode: return .distance(steps: try arg(1))
        case sizeCode: return .size(steps: try arg(1))
        case resetCode: return .reset
        default: return .unknown(command: command, args: Data(bytes.dropFirst()))
        }
    }

    public var description: String {
        func signed(_ v: Int8) -> String { v > 0 ? "+\(v)" : "\(v)" }
        switch self {
        case .recenter: return "RECENTER"
        case .move(let dx, let dy): return "MOVE dx=\(signed(dx)) dy=\(signed(dy))"
        case .distance(let steps): return "DISTANCE \(signed(steps))"
        case .size(let steps): return "SIZE \(signed(steps))"
        case .reset: return "RESET"
        case .unknown(let command, let args): return "unknown command 0x\(String(command, radix: 16)) (\(args.count) arg bytes)"
        }
    }
}
