import Foundation
import Testing
@testable import MirrorProtocol

@Test func discoveryLayouts() {
    #expect(DiscoveryMessage.discover.encoded() == Data([0x51, 0x4D, 0x49, 0x52, 0x01, 0x00, 0x01]))
    #expect(DiscoveryMessage.announce(tcpPort: 7777, name: "Mac").encoded()
        == Data([0x51, 0x4D, 0x49, 0x52, 0x02, 0x00, 0x01, 0x1E, 0x61, 0x03, 0x4D, 0x61, 0x63]))
}

@Test func discoveryRoundTripAndForwardCompatibility() {
    for m in [DiscoveryMessage.discover, .announce(tcpPort: 1, name: ""), .announce(tcpPort: 65535, name: "Mac de Zoé")] {
        #expect(DiscoveryMessage.decode(m.encoded()) == m)
        #expect(DiscoveryMessage.decode(m.encoded() + Data([0xFF, 0x00, 0x42])) == m) // trailing bytes ignored
    }
}

@Test func discoveryRejectsMalformedDatagrams() {
    let announce = DiscoveryMessage.announce(tcpPort: 7777, name: "Mac").encoded()
    var otherVersion = announce
    otherVersion[6] = 2
    var badMagic = announce
    badMagic[0] = 0x58
    var unknownType = DiscoveryMessage.discover.encoded()
    unknownType[4] = 0x03
    var nameTooLong = Data(announce.prefix(9))
    nameTooLong.append(64)
    nameTooLong.append(Data(repeating: 0x41, count: 64))
    var badUTF8 = Data(announce.prefix(9))
    badUTF8.append(contentsOf: [2, 0xC3, 0x28])
    for datagram in [Data(), Data(announce.prefix(6)), otherVersion, badMagic, unknownType,
                     Data(announce.prefix(8)), Data(announce.prefix(12)), nameTooLong, badUTF8] {
        #expect(DiscoveryMessage.decode(datagram) == nil)
    }
}

@Test func announceNameIsTruncatedAtCharacterBoundary() {
    let long = String(repeating: "a", count: 61) + "é" + "b" // 61 + 2 + 1 = 64 bytes
    #expect(DiscoveryMessage.truncatedUTF8(long).count == 63)
    let emoji = String(repeating: "a", count: 61) + "😀" // emoji is 4 bytes, does not fit
    #expect(DiscoveryMessage.truncatedUTF8(emoji) == Array(String(repeating: "a", count: 61).utf8))
    guard case .announce(_, let name)? = DiscoveryMessage.decode(DiscoveryMessage.announce(tcpPort: 1, name: emoji).encoded()) else {
        Issue.record("announce did not decode")
        return
    }
    #expect(name == String(repeating: "a", count: 61))
}
