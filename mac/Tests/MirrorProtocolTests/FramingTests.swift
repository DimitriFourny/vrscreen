import Foundation
import Testing
@testable import MirrorProtocol

private let sampleMessages: [Message] = [
    .config(ConfigMessage(codec: .hevc, width: 3840, height: 2160,
                          parameterSets: Data([0, 0, 0, 1, 0x40, 0x01, 0, 0, 0, 1, 0x42, 0x01, 0, 0, 0, 1, 0x44, 0x01]))),
    .frame(FrameMessage(isKeyframe: true, captureTimeMicros: 0x0102_0304_0506_0708,
                        accessUnit: Data((0..<5000).map { UInt8(truncatingIfNeeded: $0) }))),
    .unknown(type: 0x7F, payload: Data([0xDE, 0xAD, 0xBE, 0xEF])),
    .frame(FrameMessage(isKeyframe: false, captureTimeMicros: 42, accessUnit: Data([0, 0, 0, 1, 0x02, 0x01]))),
    .unknown(type: 0x55, payload: Data()),
    .ping(clientTimeMicros: 123_456_789),
    .pong(clientTimeMicros: 1, serverTimeMicros: .max),
    .frameAck(framesConsumed: 0xFFFF_FFFF),
    .frameAck(framesConsumed: 1),
    .config(ConfigMessage(codec: .h264, width: 1920, height: 1080, parameterSets: Data())),
]

private func stream() -> Data {
    var data = WireProtocol.helloBytes()
    for m in sampleMessages { data.append(m.encoded()) }
    return data
}

private func parse(_ data: Data, chunkSizes: [Int]) throws -> [Message] {
    var reader = MessageReader(expectHello: true)
    var out: [Message] = []
    var offset = 0
    var k = 0
    while offset < data.count {
        let n = min(chunkSizes[k % chunkSizes.count], data.count - offset)
        k += 1
        reader.append(data.subdata(in: offset..<offset + n))
        offset += n
        while let m = try reader.next() { out.append(m) }
    }
    #expect(reader.bufferedByteCount == 0)
    return out
}

@Test func helloAndHeaderLayout() {
    #expect(WireProtocol.helloBytes() == Data([0x51, 0x4D, 0x49, 0x52, 0x00, 0x01]))
    let ping = Message.ping(clientTimeMicros: 0x0A0B_0C0D_0E0F_1011).encoded()
    #expect(ping == Data([0x10, 0, 0, 0, 8, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11]))
    let frame = Message.frame(FrameMessage(isKeyframe: true, captureTimeMicros: 2, accessUnit: Data([9]))).encoded()
    #expect(frame == Data([0x02, 0, 0, 0, 10, 0x01, 0, 0, 0, 0, 0, 0, 0, 2, 9]))
    let ack = Message.frameAck(framesConsumed: 0x0102_0304).encoded()
    #expect(ack == Data([0x12, 0, 0, 0, 4, 0x01, 0x02, 0x03, 0x04]))
}

@Test func frameAckPayloadRules() throws {
    // Bytes beyond the defined field are ignored (forward compatibility).
    #expect(try Message.decode(type: 0x12, payload: Data([0, 0, 0, 7, 0xAA])) == .frameAck(framesConsumed: 7))
    #expect(throws: ProtocolError.self) { try Message.decode(type: 0x12, payload: Data([0, 0, 7])) }
}

@Test(arguments: [[Int.max], [1], [2, 3], [5], [6], [7, 1, 4096], [13, 1, 1, 999]])
func roundTripWithSplitReads(chunkSizes: [Int]) throws {
    #expect(try parse(stream(), chunkSizes: chunkSizes) == sampleMessages)
}

@Test func incompleteMessageWaitsForMoreBytes() throws {
    let data = stream()
    var reader = MessageReader(expectHello: true)
    reader.append(data.prefix(WireProtocol.helloSize + WireProtocol.headerSize + 3))
    #expect(try reader.next() == nil)
    #expect(reader.peerVersion == 1)
    reader.append(data.dropFirst(WireProtocol.helloSize + WireProtocol.headerSize + 3))
    #expect(try reader.next() == sampleMessages[0])
}

@Test func rejectsBadHello() {
    var badMagic = MessageReader(expectHello: true)
    badMagic.append(Data("QMIX".utf8) + Data([0, 1]))
    #expect(throws: ProtocolError.badMagic(Array("QMIX".utf8))) { try badMagic.next() }

    var badVersion = MessageReader(expectHello: true)
    badVersion.append(Data("QMIR".utf8) + Data([0, 2]))
    #expect(throws: ProtocolError.unsupportedVersion(2)) { try badVersion.next() }
}

@Test func rejectsOversizedAndShortPayloads() {
    var big = MessageReader(expectHello: false)
    big.append(Data([0x02, 0xFF, 0xFF, 0xFF, 0xFF]))
    #expect(throws: ProtocolError.payloadTooLarge(type: 0x02, length: 0xFFFF_FFFF)) { try big.next() }

    var short = MessageReader(expectHello: false)
    short.append(Data([0x11, 0, 0, 0, 8, 1, 2, 3, 4, 5, 6, 7, 8]))
    #expect(throws: ProtocolError.self) { try short.next() }
}
