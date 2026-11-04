import Foundation
import MirrorProtocol
import Network

/// Estimates the offset between the sender clock and the local clock from
/// PING/PONG round trips, using the sample with the smallest RTT among the recent ones.
struct ClockSync {
    private var samples: [(rtt: Int64, offset: Int64)] = []
    private static let window = 16
    private(set) var lastRTTMicros: Int64?

    mutating func reset() {
        samples.removeAll()
        lastRTTMicros = nil
    }

    mutating func addPong(clientSendMicros: UInt64, serverMicros: UInt64, clientReceiveMicros: UInt64) {
        let rtt = Int64(clientReceiveMicros) - Int64(clientSendMicros)
        guard rtt >= 0 else { return }
        // Assume the server stamped the reply halfway through the round trip.
        let offset = Int64(serverMicros) - (Int64(clientSendMicros) + rtt / 2)
        samples.append((rtt, offset))
        if samples.count > Self.window { samples.removeFirst() }
        lastRTTMicros = rtt
    }

    /// serverClock - localClock, in µs.
    var offsetMicros: Int64? { samples.min { $0.rtt < $1.rtt }?.offset }

    /// The current time expressed on the sender's clock.
    func serverNowMicros() -> Int64? {
        guard let offsetMicros else { return nil }
        return Int64(HostClock.nowMicros()) + offsetMicros
    }
}

/// TCP client for the mirror protocol. Reconnects every second while the sender is
/// unreachable; with a `Discoverer`, each (re)connect first finds the sender on the LAN.
/// All callbacks and state live on `queue`.
final class ViewerClient {
    private(set) var host: String
    private(set) var port: UInt16
    let queue: DispatchQueue
    private let discoverer: Discoverer?
    /// CONFIG / FRAME / CONTROL / AUDIO_CONFIG / AUDIO messages, in stream order.
    var onMessage: ((Message) -> Void)?
    var onDisconnected: (() -> Void)?

    private(set) var clock = ClockSync()
    private(set) var bytesReceived = 0

    private var connection: NWConnection?
    private var generation = 0
    private var reader = MessageReader(expectHello: true)
    private var pingTimer: DispatchSourceTimer?
    private var connected = false
    /// FRAMEs consumed on the current connection (FRAME_ACK counter, wraps).
    private var framesConsumed: UInt32 = 0
    private var reportedOutage = false

    /// Connects to `host:port`, or, if `discoverer` is given, to whichever sender
    /// answers discovery first (host/port are then filled in on each attempt).
    init(host: String, port: UInt16, discoverer: Discoverer?, queue: DispatchQueue) {
        self.host = host
        self.port = port
        self.discoverer = discoverer
        self.queue = queue
    }

    func start() {
        queue.async { self.beginConnect() }
    }

    private func beginConnect() {
        guard let discoverer else { return connect() }
        discoverer.discover { [weak self] host, port, name in
            guard let self else { return }
            log("client: discovered \"\(name)\" at \(host), TCP port \(port)")
            self.host = host
            self.port = port
            self.connect()
        }
    }

    private func connect() {
        generation += 1
        let gen = generation
        reader = MessageReader(expectHello: true)
        clock.reset()
        framesConsumed = 0
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        let conn = NWConnection(host: NWEndpoint.Host(host), port: NWEndpoint.Port(rawValue: port)!,
                                using: NWParameters(tls: nil, tcp: tcp))
        connection = conn
        conn.stateUpdateHandler = { [weak self] state in
            guard let self, gen == self.generation else { return }
            switch state {
            case .ready:
                self.connected = true
                self.reportedOutage = false
                log("client: connected to \(self.host):\(self.port)")
                self.receive(conn, gen)
                self.startPings(conn)
            case .waiting(let error), .failed(let error):
                self.fail("\(error)")
            default:
                break
            }
        }
        conn.start(queue: queue)
    }

    private func fail(_ reason: String) {
        guard let conn = connection else { return }
        connection = nil
        generation += 1
        conn.cancel()
        pingTimer?.cancel()
        pingTimer = nil
        if connected {
            connected = false
            onDisconnected?()
        }
        if !reportedOutage {
            reportedOutage = true
            log("client: disconnected (\(reason)); retrying every 1 s")
        }
        queue.asyncAfter(deadline: .now() + 1) { [weak self] in self?.beginConnect() }
    }

    private func receive(_ conn: NWConnection, _ gen: Int) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 1 << 20) { [weak self] data, _, isComplete, error in
            guard let self, gen == self.generation else { return }
            if let data, !data.isEmpty {
                self.bytesReceived += data.count
                self.reader.append(data)
                do {
                    while let message = try self.reader.next() {
                        self.handle(message, conn)
                    }
                } catch {
                    return self.fail("protocol error: \(error)")
                }
            }
            if let error {
                self.fail("\(error)")
            } else if isComplete {
                self.fail("closed by sender")
            } else {
                self.receive(conn, gen)
            }
        }
    }

    private func handle(_ message: Message, _ conn: NWConnection) {
        switch message {
        case .pong(let clientTime, let serverTime):
            clock.addPong(clientSendMicros: clientTime, serverMicros: serverTime,
                          clientReceiveMicros: HostClock.nowMicros())
        case .config, .frame, .control, .audioConfig, .audio:
            onMessage?(message)
        case .ping, .frameAck:
            break // Client-to-server types.
        case .unknown:
            break // Forward compatibility: skip unknown types.
        }
    }

    /// Reports one more FRAME of this connection as consumed (handed to the decoder
    /// or discarded) with a FRAME_ACK, so the sender can bound frames in flight.
    func acknowledgeFrame() {
        guard let conn = connection, connected else { return }
        framesConsumed &+= 1
        conn.send(content: Message.frameAck(framesConsumed: framesConsumed).encoded(),
                  completion: .contentProcessed { _ in })
    }

    /// One PING right away and a few more quickly for a good first offset, then 1/s.
    private func startPings(_ conn: NWConnection) {
        let timer = DispatchSource.makeTimerSource(queue: queue)
        var sent = 0
        timer.schedule(deadline: .now(), repeating: .milliseconds(100))
        timer.setEventHandler { [weak timer] in
            guard let timer else { return }
            sent += 1
            if sent == 5 { timer.schedule(deadline: .now() + 1, repeating: 1) }
            conn.send(content: Message.ping(clientTimeMicros: HostClock.nowMicros()).encoded(),
                      completion: .contentProcessed { _ in })
        }
        timer.resume()
        pingTimer = timer
    }
}
