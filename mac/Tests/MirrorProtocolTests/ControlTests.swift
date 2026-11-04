import Foundation
import Testing
@testable import MirrorProtocol

@Test func controlLayouts() {
    #expect(Message.control(.recenter).encoded() == Data([0x03, 0, 0, 0, 1, 0x01]))
    #expect(Message.control(.move(dx: -1, dy: 2)).encoded() == Data([0x03, 0, 0, 0, 3, 0x02, 0xFF, 0x02]))
    #expect(Message.control(.distance(steps: 1)).encoded() == Data([0x03, 0, 0, 0, 2, 0x03, 0x01]))
    #expect(Message.control(.size(steps: -128)).encoded() == Data([0x03, 0, 0, 0, 2, 0x04, 0x80]))
    #expect(Message.control(.reset).encoded() == Data([0x03, 0, 0, 0, 1, 0x05]))
}

@Test func controlRoundTripThroughStream() throws {
    let commands: [ControlCommand] = [.recenter, .move(dx: 127, dy: -128), .distance(steps: -3), .size(steps: 4), .reset,
                                      .unknown(command: 0x7E, args: Data([1, 2, 3]))]
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
    #expect(try ControlCommand.decode(Data([0x02, 0x01, 0xFF, 0xAA, 0xBB])) == .move(dx: 1, dy: -1))
    #expect(try ControlCommand.decode(Data([0x01, 0x09])) == .recenter)
    // Unknown commands are surfaced as such (receivers ignore them), not as errors.
    #expect(try ControlCommand.decode(Data([0x42])) == .unknown(command: 0x42, args: Data()))
    // Missing arguments or an empty payload are malformed.
    #expect(throws: ProtocolError.self) { try ControlCommand.decode(Data()) }
    #expect(throws: ProtocolError.self) { try ControlCommand.decode(Data([0x02, 0x01])) }
    #expect(throws: ProtocolError.self) { try ControlCommand.decode(Data([0x04])) }
}
