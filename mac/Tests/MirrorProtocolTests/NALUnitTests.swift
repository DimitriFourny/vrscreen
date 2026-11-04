import Foundation
import Testing
@testable import MirrorProtocol

@Test func lengthPrefixedToAnnexB() throws {
    let avcc = Data([0, 0, 0, 3, 0x65, 0xAA, 0xBB, 0, 0, 0, 1, 0x41, 0, 0, 0, 0])
    let annexB = try NALUnits.annexB(fromLengthPrefixed: avcc)
    #expect(annexB == Data([0, 0, 0, 1, 0x65, 0xAA, 0xBB, 0, 0, 0, 1, 0x41, 0, 0, 0, 1]))

    let twoByte = Data([0, 2, 0x67, 0x42, 0, 1, 0x68])
    #expect(try NALUnits.annexB(fromLengthPrefixed: twoByte, lengthSize: 2)
        == Data([0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68]))

    // Works on a slice whose indices do not start at 0.
    let sliced = (Data([0xFF, 0xFF]) + avcc).dropFirst(2)
    #expect(try NALUnits.annexB(fromLengthPrefixed: sliced) == annexB)
}

@Test func lengthPrefixedRejectsTruncation() {
    #expect(throws: NALUnits.ConversionError.truncated(offset: 4)) {
        try NALUnits.annexB(fromLengthPrefixed: Data([0, 0, 0, 9, 1, 2]))
    }
    #expect(throws: NALUnits.ConversionError.truncated(offset: 0)) {
        try NALUnits.annexB(fromLengthPrefixed: Data([0, 0]))
    }
}

@Test func splitAnnexBHandlesStartCodeVariants() {
    // Leading garbage, 4-byte and 3-byte start codes, trailing_zero_8bits, emulation
    // prevention (00 00 03) and a payload containing 00 00 02 must not split.
    let data = Data([0xAB,
                     0, 0, 0, 1, 0x40, 0x01, 0x0C,
                     0, 0, 1, 0x42, 0x01, 0x00, 0x00, 0x03, 0x01,
                     0, 0, 0, 0, 1, 0x26, 0x01, 0x00, 0x00, 0x02, 0x7F,
                     0, 0])
    #expect(NALUnits.split(annexB: data) == [
        Data([0x40, 0x01, 0x0C]),
        Data([0x42, 0x01, 0x00, 0x00, 0x03, 0x01]),
        Data([0x26, 0x01, 0x00, 0x00, 0x02, 0x7F]),
    ])
    #expect(NALUnits.split(annexB: Data([0, 0, 1])) == [])
    #expect(NALUnits.split(annexB: Data()) == [])
}

@Test func joinAndSplitRoundTrip() throws {
    let nals = [Data([0x67, 1, 2, 3]), Data([0x68, 0xCE]), Data([0x65] + [UInt8](repeating: 0x11, count: 300))]
    #expect(NALUnits.split(annexB: NALUnits.annexB(joining: nals)) == nals)
    #expect(try NALUnits.annexB(fromLengthPrefixed: NALUnits.lengthPrefixed(nals)) == NALUnits.annexB(joining: nals))
}

@Test func nalTypes() {
    #expect(NALUnits.type(of: Data([0x40, 0x01]), codec: .hevc) == 32)
    #expect(NALUnits.type(of: Data([0x26, 0x01]), codec: .hevc) == 19)
    #expect(NALUnits.type(of: Data([0x67]), codec: .h264) == 7)
    #expect(NALUnits.isParameterSet(Data([0x44, 0x01]), codec: .hevc))
    #expect(!NALUnits.isParameterSet(Data([0x65]), codec: .h264))
}
