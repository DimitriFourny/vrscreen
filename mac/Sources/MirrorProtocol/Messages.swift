import Foundation

/// Wire protocol v1 constants. See `docs/protocol.md` (the binding contract).
public enum WireProtocol {
    /// Hello magic sent by the server right after the TCP connection is established.
    public static let magic: [UInt8] = Array("VRSC".utf8)
    public static let version: UInt16 = 1
    /// `magic (4) | version u16`
    public static let helloSize = 6
    /// `type u8 | length u32`
    public static let headerSize = 5
    public static let defaultPort: UInt16 = 7777
    /// Receivers reject messages with a larger payload as a protocol error.
    public static let maxPayloadLength: UInt32 = 64 << 20

    public static func helloBytes() -> Data {
        var data = Data(magic)
        data.appendBigEndian(version)
        return data
    }
}

public enum VideoCodec: UInt8, Sendable, CustomStringConvertible {
    case h264 = 1
    case hevc = 2

    public var description: String {
        switch self {
        case .h264: "H.264"
        case .hevc: "HEVC"
        }
    }
}

public enum MessageType: UInt8, Sendable {
    case config = 0x01
    case frame = 0x02
    case control = 0x03
    case audioConfig = 0x04
    case audio = 0x05
    case ping = 0x10
    case pong = 0x11
    case frameAck = 0x12
    case headsetStatus = 0x13
}

/// `0x01 CONFIG`: `codec u8 | width u32 | height u32 | parameter sets (Annex-B)`.
public struct ConfigMessage: Equatable, Sendable {
    public var codec: VideoCodec
    public var width: UInt32
    public var height: UInt32
    /// Annex-B (`00 00 00 01` start codes): HEVC VPS+SPS+PPS or H.264 SPS+PPS.
    public var parameterSets: Data

    public init(codec: VideoCodec, width: UInt32, height: UInt32, parameterSets: Data) {
        self.codec = codec
        self.width = width
        self.height = height
        self.parameterSets = parameterSets
    }
}

/// `0x02 FRAME`: `flags u8 | captureTimeMicros u64 | Annex-B access unit`.
public struct FrameMessage: Equatable, Sendable {
    public static let keyframeFlag: UInt8 = 0x01

    public var isKeyframe: Bool
    /// Sender host clock (mach absolute time) in microseconds.
    public var captureTimeMicros: UInt64
    /// Annex-B access unit; keyframes start with the parameter sets.
    public var accessUnit: Data

    public init(isKeyframe: Bool, captureTimeMicros: UInt64, accessUnit: Data) {
        self.isKeyframe = isKeyframe
        self.captureTimeMicros = captureTimeMicros
        self.accessUnit = accessUnit
    }
}

public enum Message: Equatable, Sendable {
    case config(ConfigMessage)
    case frame(FrameMessage)
    /// Server → client: screen placement command.
    case control(ControlCommand)
    /// Server → client: format of the following AUDIO messages.
    case audioConfig(AudioConfigMessage)
    /// Server → client: one chunk of PCM audio.
    case audio(AudioMessage)
    /// Client → server.
    case ping(clientTimeMicros: UInt64)
    /// Server → client; `serverTimeMicros` is on the same clock as `captureTimeMicros`.
    case pong(clientTimeMicros: UInt64, serverTimeMicros: UInt64)
    /// Client → server: cumulative number of FRAMEs on this connection the client
    /// has consumed (handed to its decoder or discarded). Wraps at 2^32.
    case frameAck(framesConsumed: UInt32)
    /// Client → server: the headset's battery and charging state.
    case headsetStatus(HeadsetStatus)
    /// A message type this implementation does not know; receivers skip it.
    case unknown(type: UInt8, payload: Data)

    /// The full framed message: `type u8 | length u32 | payload`.
    public func encoded() -> Data {
        var payload = Data()
        let type: UInt8
        switch self {
        case .config(let c):
            type = MessageType.config.rawValue
            payload.reserveCapacity(9 + c.parameterSets.count)
            payload.append(c.codec.rawValue)
            payload.appendBigEndian(c.width)
            payload.appendBigEndian(c.height)
            payload.append(c.parameterSets)
        case .frame(let f):
            type = MessageType.frame.rawValue
            payload.reserveCapacity(9 + f.accessUnit.count)
            payload.append(f.isKeyframe ? FrameMessage.keyframeFlag : 0)
            payload.appendBigEndian(f.captureTimeMicros)
            payload.append(f.accessUnit)
        case .ping(let t):
            type = MessageType.ping.rawValue
            payload.appendBigEndian(t)
        case .pong(let c, let s):
            type = MessageType.pong.rawValue
            payload.appendBigEndian(c)
            payload.appendBigEndian(s)
        case .frameAck(let n):
            type = MessageType.frameAck.rawValue
            payload.appendBigEndian(n)
        case .headsetStatus(let s):
            type = MessageType.headsetStatus.rawValue
            payload.append(s.batteryPercent)
            payload.append(s.flags)
        case .control(let command):
            type = MessageType.control.rawValue
            payload = command.encoded()
        case .audioConfig(let c):
            type = MessageType.audioConfig.rawValue
            payload.append(c.format)
            payload.appendBigEndian(c.sampleRate)
            payload.append(c.channels)
        case .audio(let a):
            type = MessageType.audio.rawValue
            payload.reserveCapacity(8 + a.pcm.count)
            payload.appendBigEndian(a.captureTimeMicros)
            payload.append(a.pcm)
        case .unknown(let t, let p):
            type = t
            payload = p
        }
        var out = Data(capacity: WireProtocol.headerSize + payload.count)
        out.append(type)
        out.appendBigEndian(UInt32(payload.count))
        out.append(payload)
        return out
    }

    /// Parses the payload of a message whose header has already been read.
    public static func decode(type: UInt8, payload: Data) throws(ProtocolError) -> Message {
        var r = ByteReader(payload)
        guard let known = MessageType(rawValue: type) else {
            return .unknown(type: type, payload: payload)
        }
        switch known {
        case .config:
            guard let codecByte = r.u8(), let width = r.u32(), let height = r.u32() else {
                throw .malformed(type: type, reason: "CONFIG payload shorter than 9 bytes")
            }
            guard let codec = VideoCodec(rawValue: codecByte) else {
                throw .malformed(type: type, reason: "unknown codec \(codecByte)")
            }
            return .config(ConfigMessage(codec: codec, width: width, height: height, parameterSets: r.rest()))
        case .frame:
            guard let flags = r.u8(), let time = r.u64() else {
                throw .malformed(type: type, reason: "FRAME payload shorter than 9 bytes")
            }
            return .frame(FrameMessage(
                isKeyframe: flags & FrameMessage.keyframeFlag != 0,
                captureTimeMicros: time,
                accessUnit: r.rest()))
        case .control:
            return .control(try ControlCommand.decode(payload))
        case .audioConfig:
            guard let format = r.u8(), let rate = r.u32(), let channels = r.u8() else {
                throw .malformed(type: type, reason: "AUDIO_CONFIG payload shorter than 6 bytes")
            }
            return .audioConfig(AudioConfigMessage(format: format, sampleRate: rate, channels: channels))
        case .audio:
            guard let time = r.u64() else { throw .malformed(type: type, reason: "AUDIO payload shorter than 8 bytes") }
            return .audio(AudioMessage(captureTimeMicros: time, pcm: r.rest()))
        case .ping:
            guard let t = r.u64() else { throw .malformed(type: type, reason: "PING payload shorter than 8 bytes") }
            return .ping(clientTimeMicros: t)
        case .pong:
            guard let c = r.u64(), let s = r.u64() else {
                throw .malformed(type: type, reason: "PONG payload shorter than 16 bytes")
            }
            return .pong(clientTimeMicros: c, serverTimeMicros: s)
        case .frameAck:
            guard let n = r.u32() else {
                throw .malformed(type: type, reason: "FRAME_ACK payload shorter than 4 bytes")
            }
            return .frameAck(framesConsumed: n)
        case .headsetStatus:
            guard let battery = r.u8(), let flags = r.u8() else {
                throw .malformed(type: type, reason: "HEADSET_STATUS payload shorter than 2 bytes")
            }
            return .headsetStatus(HeadsetStatus(batteryPercent: battery, flags: flags))
        }
    }
}

public enum ProtocolError: Error, Equatable, CustomStringConvertible {
    case badMagic([UInt8])
    case unsupportedVersion(UInt16)
    case payloadTooLarge(type: UInt8, length: UInt32)
    case malformed(type: UInt8, reason: String)

    public var description: String {
        switch self {
        case .badMagic(let m): "bad hello magic \(m.map { String(format: "%02x", $0) }.joined())"
        case .unsupportedVersion(let v): "unsupported protocol version \(v)"
        case .payloadTooLarge(let t, let l): "message type 0x\(String(t, radix: 16)) payload \(l) bytes exceeds limit"
        case .malformed(let t, let reason): "malformed message type 0x\(String(t, radix: 16)): \(reason)"
        }
    }
}

/// Incremental parser for a TCP byte stream: feed arbitrary chunks with `append`,
/// then drain complete messages with `next()`. Partial headers/payloads are buffered.
public struct MessageReader: Sendable {
    private var buffer: [UInt8] = []
    private var readIndex = 0
    private var awaitingHello: Bool
    /// Version from the server hello, once received (client side only).
    public private(set) var peerVersion: UInt16?

    /// - Parameter expectHello: true on the client side (the stream starts with `VRSC | version`).
    public init(expectHello: Bool) {
        awaitingHello = expectHello
    }

    public var bufferedByteCount: Int { buffer.count - readIndex }

    public mutating func append(_ data: Data) {
        if readIndex > 0 && readIndex >= buffer.count / 2 {
            buffer.removeFirst(readIndex)
            readIndex = 0
        }
        buffer.append(contentsOf: data)
    }

    /// Returns the next complete message, or nil if more bytes are needed.
    /// Unknown message types are returned as `.unknown` (already skipped in the stream).
    public mutating func next() throws(ProtocolError) -> Message? {
        if awaitingHello {
            guard bufferedByteCount >= WireProtocol.helloSize else { return nil }
            let magic = Array(buffer[readIndex..<readIndex + 4])
            guard magic == WireProtocol.magic else { throw .badMagic(magic) }
            let version = UInt16(buffer[readIndex + 4]) << 8 | UInt16(buffer[readIndex + 5])
            guard version == WireProtocol.version else { throw .unsupportedVersion(version) }
            peerVersion = version
            readIndex += WireProtocol.helloSize
            awaitingHello = false
        }
        guard bufferedByteCount >= WireProtocol.headerSize else { return nil }
        let type = buffer[readIndex]
        var length: UInt32 = 0
        for i in 1...4 { length = length << 8 | UInt32(buffer[readIndex + i]) }
        guard length <= WireProtocol.maxPayloadLength else { throw .payloadTooLarge(type: type, length: length) }
        let start = readIndex + WireProtocol.headerSize
        let end = start + Int(length)
        guard buffer.count >= end else { return nil }
        readIndex = end
        let payload = buffer.withUnsafeBufferPointer { Data(UnsafeBufferPointer(rebasing: $0[start..<end])) }
        return try Message.decode(type: type, payload: payload)
    }
}

struct ByteReader {
    private let data: Data
    private var offset: Int

    init(_ data: Data) {
        self.data = data
        offset = data.startIndex
    }

    private mutating func integer<T: FixedWidthInteger>(_: T.Type) -> T? {
        let size = MemoryLayout<T>.size
        guard data.endIndex - offset >= size else { return nil }
        var value: T = 0
        for i in 0..<size { value = value << 8 | T(data[offset + i]) }
        offset += size
        return value
    }

    mutating func u8() -> UInt8? { integer(UInt8.self) }
    mutating func u16() -> UInt16? { integer(UInt16.self) }
    mutating func u32() -> UInt32? { integer(UInt32.self) }
    mutating func u64() -> UInt64? { integer(UInt64.self) }

    mutating func rest() -> Data {
        defer { offset = data.endIndex }
        return Data(data[offset...])
    }
}

extension Data {
    mutating func appendBigEndian<T: FixedWidthInteger>(_ value: T) {
        Swift.withUnsafeBytes(of: value.bigEndian) { append(contentsOf: $0) }
    }
}
