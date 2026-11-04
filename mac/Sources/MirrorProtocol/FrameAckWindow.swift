/// Server-side bookkeeping for FRAME_ACK flow control (docs/protocol.md, "Flow control").
///
/// Counts FRAME messages sent on one connection and the client's cumulative
/// `framesConsumed`. Until the client's first FRAME_ACK the window is always open
/// (clients that never ack are only limited by the socket backlog); afterwards at
/// most `limit` frames may be sent but not yet acknowledged. Counters are
/// cumulative `u32` values that wrap, like the wire field.
public struct FrameAckWindow: Sendable {
    public let limit: UInt32
    public private(set) var framesSent: UInt32
    public private(set) var framesAcked: UInt32
    /// The client has sent at least one valid FRAME_ACK.
    public private(set) var isActive = false

    public init(limit: UInt32) {
        self.init(limit: limit, framesSent: 0, framesAcked: 0)
    }

    init(limit: UInt32, framesSent: UInt32, framesAcked: UInt32) {
        precondition(limit > 0, "a zero window would never let a frame out")
        self.limit = limit
        self.framesSent = framesSent
        self.framesAcked = framesAcked
    }

    /// Frames sent but not yet acknowledged.
    public var framesInFlight: UInt32 { framesSent &- framesAcked }

    /// Whether another FRAME may be sent now.
    public var isOpen: Bool { !isActive || framesInFlight < limit }

    public mutating func recordSent() {
        framesSent &+= 1
    }

    /// Applies a FRAME_ACK. Returns false (and changes nothing) if `framesConsumed`
    /// is not between the previous ack and the number of frames sent, i.e. it goes
    /// backwards or acknowledges frames that were never sent.
    @discardableResult
    public mutating func recordAck(framesConsumed: UInt32) -> Bool {
        guard framesConsumed &- framesAcked <= framesInFlight else { return false }
        framesAcked = framesConsumed
        isActive = true
        return true
    }
}
