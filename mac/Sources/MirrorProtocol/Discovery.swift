import Foundation

/// LAN discovery datagrams (UDP, docs/protocol.md "Discovery").
///
/// `"QMIR" | type u8 | version u16 | payload`. Clients broadcast DISCOVER; senders
/// answer each one with a unicast ANNOUNCE. Trailing bytes are ignored.
public enum DiscoveryMessage: Equatable, Sendable {
    public static let defaultPort: UInt16 = 7778
    public static let version: UInt16 = 1
    public static let discoverType: UInt8 = 0x01
    public static let announceType: UInt8 = 0x02
    /// Longest name carried by ANNOUNCE, in UTF-8 bytes.
    public static let maxNameBytes = 63

    case discover
    /// `tcpPort`: where the sender's TCP server listens; `name`: human-readable Mac name.
    case announce(tcpPort: UInt16, name: String)

    public func encoded() -> Data {
        var data = Data(WireProtocol.magic)
        switch self {
        case .discover:
            data.append(Self.discoverType)
            data.appendBigEndian(Self.version)
        case .announce(let tcpPort, let name):
            let nameBytes = Self.truncatedUTF8(name)
            data.append(Self.announceType)
            data.appendBigEndian(Self.version)
            data.appendBigEndian(tcpPort)
            data.append(UInt8(nameBytes.count))
            data.append(contentsOf: nameBytes)
        }
        return data
    }

    /// Parses a datagram; nil for anything that is not a well-formed v1 message.
    public static func decode(_ datagram: Data) -> DiscoveryMessage? {
        var r = ByteReader(datagram)
        guard datagram.count >= 7, Array(datagram.prefix(4)) == WireProtocol.magic else { return nil }
        _ = r.u32()
        guard let type = r.u8(), let version = r.u16(), version == Self.version else { return nil }
        switch type {
        case discoverType:
            return .discover
        case announceType:
            guard let port = r.u16(), let length = r.u8() else { return nil }
            let rest = r.rest()
            guard Int(length) <= min(rest.count, maxNameBytes),
                  let name = String(data: rest.prefix(Int(length)), encoding: .utf8) else { return nil }
            return .announce(tcpPort: port, name: name)
        default:
            return nil
        }
    }

    /// UTF-8 bytes of `name`, cut at a character boundary to fit `maxNameBytes`.
    static func truncatedUTF8(_ name: String) -> [UInt8] {
        var bytes: [UInt8] = []
        for character in name {
            let utf8 = Array(String(character).utf8)
            guard bytes.count + utf8.count <= maxNameBytes else { break }
            bytes += utf8
        }
        return bytes
    }
}
