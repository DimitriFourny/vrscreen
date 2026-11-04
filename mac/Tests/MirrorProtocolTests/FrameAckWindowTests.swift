import Testing
@testable import MirrorProtocol

/// `#expect` cannot wrap a mutating call, so apply the ack first.
private func ack(_ w: inout FrameAckWindow, _ n: UInt32) -> Bool {
    w.recordAck(framesConsumed: n)
}

@Test func windowIsOpenUntilFirstAck() {
    var w = FrameAckWindow(limit: 2)
    for _ in 0..<10 { w.recordSent() }
    #expect(w.isOpen && !w.isActive)
    let first = ack(&w, 7)
    #expect(first && w.isActive && w.framesInFlight == 3 && !w.isOpen)
    let second = ack(&w, 9)
    #expect(second && w.framesInFlight == 1 && w.isOpen)
    w.recordSent()
    #expect(w.framesInFlight == 2 && !w.isOpen)
}

@Test func firstAckOfZeroActivatesWindow() {
    var w = FrameAckWindow(limit: 1)
    w.recordSent()
    let accepted = ack(&w, 0)
    #expect(accepted && w.isActive && !w.isOpen)
}

@Test func rejectsAcksOutsideSentRange() {
    var w = FrameAckWindow(limit: 3)
    for _ in 0..<5 { w.recordSent() }
    let tooMany = ack(&w, 6)
    #expect(!tooMany && !w.isActive)
    let valid = ack(&w, 4)
    let backwards = ack(&w, 3)
    #expect(valid && !backwards && w.framesAcked == 4)
    let repeated = ack(&w, 4)
    #expect(repeated)
}

@Test func countersWrapAt32Bits() {
    var w = FrameAckWindow(limit: 3, framesSent: .max - 1, framesAcked: .max - 1)
    let initial = ack(&w, .max - 1)
    #expect(initial)
    for _ in 0..<3 { w.recordSent() } // framesSent wraps to 1
    #expect(w.framesSent == 1 && w.framesInFlight == 3 && !w.isOpen)
    let wrapped = ack(&w, 0)
    #expect(wrapped && w.framesInFlight == 1 && w.isOpen)
    let beyondSent = ack(&w, 2)
    #expect(!beyondSent)
}
