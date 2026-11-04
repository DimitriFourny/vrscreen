import Foundation
import Testing
@testable import MirrorProtocol

@Test func controlLayouts() {
    #expect(Message.control(.recenter).encoded() == Data([0x03, 0, 0, 0, 1, 0x01]))
    #expect(Message.control(.distance(steps: 1)).encoded() == Data([0x03, 0, 0, 0, 2, 0x03, 0x01]))
    #expect(Message.control(.size(steps: -128)).encoded() == Data([0x03, 0, 0, 0, 2, 0x04, 0x80]))
    #expect(Message.control(.reset).encoded() == Data([0x03, 0, 0, 0, 1, 0x05]))
    #expect(Message.control(.translate(dx: -1, dy: 2)).encoded() == Data([0x03, 0, 0, 0, 3, 0x06, 0xFF, 0x02]))
    // ORBIT yaw +5.00° (500 = 0x01F4), pitch -2.50° (-250 = 0xFF06), big-endian.
    #expect(Message.control(.orbit(yawCentidegrees: 500, pitchCentidegrees: -250)).encoded()
        == Data([0x03, 0, 0, 0, 5, 0x07, 0x01, 0xF4, 0xFF, 0x06]))
    #expect(Message.control(.distanceFine(-25)).encoded() == Data([0x03, 0, 0, 0, 3, 0x08, 0xFF, 0xE7]))
    #expect(Message.control(.sizeFine(.max)).encoded() == Data([0x03, 0, 0, 0, 3, 0x09, 0x7F, 0xFF]))
}

@Test func controlRoundTripThroughStream() throws {
    let commands: [ControlCommand] = [
        .recenter, .distance(steps: -3), .size(steps: 4), .reset, .translate(dx: 127, dy: -128),
        .orbit(yawCentidegrees: .min, pitchCentidegrees: .max), .distanceFine(100), .sizeFine(-1),
        .unknown(command: 0x7E, args: Data([1, 2, 3])),
    ]
    var stream = WireProtocol.helloBytes()
    for c in commands { stream.append(Message.control(c).encoded()) }
    var reader = MessageReader(expectHello: true)
    var decoded: [Message] = []
    for byte in stream { // one byte at a time
        reader.append(Data([byte]))
        while let m = try reader.next() { decoded.append(m) }
    }
    #expect(decoded == commands.map { .control($0) })
}

@Test func controlForwardCompatibility() throws {
    // Extra bytes after a known command's arguments are ignored.
    #expect(try ControlCommand.decode(Data([0x06, 0x01, 0xFF, 0xAA, 0xBB])) == .translate(dx: 1, dy: -1))
    #expect(try ControlCommand.decode(Data([0x08, 0x00, 0x19, 0x09])) == .distanceFine(25))
    #expect(try ControlCommand.decode(Data([0x01, 0x09])) == .recenter)
    // 0x02 (the removed MOVE) is reserved: decoded as unknown, so receivers ignore it.
    #expect(try ControlCommand.decode(Data([0x02, 0x01, 0x00])) == .unknown(command: 0x02, args: Data([0x01, 0x00])))
    #expect(try ControlCommand.decode(Data([0x42])) == .unknown(command: 0x42, args: Data()))
    // Missing arguments or an empty payload are malformed.
    #expect(throws: ProtocolError.self) { try ControlCommand.decode(Data()) }
    #expect(throws: ProtocolError.self) { try ControlCommand.decode(Data([0x06, 0x01])) }
    #expect(throws: ProtocolError.self) { try ControlCommand.decode(Data([0x07, 0x00, 0x01, 0x00])) }
    #expect(throws: ProtocolError.self) { try ControlCommand.decode(Data([0x09, 0x00])) }
    #expect(throws: ProtocolError.self) { try ControlCommand.decode(Data([0x04])) }
}
