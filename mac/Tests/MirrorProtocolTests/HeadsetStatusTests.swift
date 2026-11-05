import Foundation
import Testing
@testable import MirrorProtocol

@Test func headsetStatusLayout() throws {
    // The docs/protocol.md example: 72 %, charging.
    let status = HeadsetStatus(batteryPercent: 72, charging: true, pluggedIn: false)
    #expect(Message.headsetStatus(status).encoded() == Data([0x13, 0, 0, 0, 2, 0x48, 0x01]))
    #expect(Message.headsetStatus(HeadsetStatus(batteryPercent: nil, charging: false, pluggedIn: true)).encoded()
        == Data([0x13, 0, 0, 0, 2, 0xFF, 0x02]))
    #expect(try Message.decode(type: 0x13, payload: Data([0x48, 0x01])) == .headsetStatus(status))
}

@Test func headsetStatusFields() {
    let full = HeadsetStatus(batteryPercent: 100, flags: 0x03)
    #expect(full.battery == 100 && full.isCharging && full.isPluggedIn)
    #expect(HeadsetStatus(batteryPercent: 255, flags: 0).battery == nil)
    #expect(HeadsetStatus(batteryPercent: 101, flags: 0).battery == nil) // out of range = unknown
    // Reserved flag bits are ignored.
    let reserved = HeadsetStatus(batteryPercent: 5, flags: 0xFC)
    #expect(reserved.battery == 5 && !reserved.isCharging && !reserved.isPluggedIn)
}

@Test func headsetStatusDecodeRules() throws {
    // Extra bytes are ignored (forward compatibility).
    #expect(try Message.decode(type: 0x13, payload: Data([0x10, 0x02, 0xAA, 0xBB]))
        == .headsetStatus(HeadsetStatus(batteryPercent: 16, flags: 0x02)))
    #expect(throws: ProtocolError.malformed(type: 0x13, reason: "HEADSET_STATUS payload shorter than 2 bytes")) {
        try Message.decode(type: 0x13, payload: Data([0x48]))
    }
    // A malformed HEADSET_STATUS has already been consumed when the reader throws, so a
    // receiver that ignores it continues with the next message.
    var reader = MessageReader(expectHello: false)
    reader.append(Data([0x13, 0, 0, 0, 1, 0x48]) + Message.ping(clientTimeMicros: 7).encoded())
    #expect(throws: ProtocolError.self) { try reader.next() }
    #expect(try reader.next() == .ping(clientTimeMicros: 7))
    #expect(try reader.next() == nil)
}
