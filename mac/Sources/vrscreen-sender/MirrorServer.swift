import Darwin
import Foundation
import MirrorProtocol

/// One connected client. All state is confined to the server queue.
///
/// Uses a BSD socket rather than NWConnection: NWConnection reports a send as
/// processed once the bytes are in the kernel, and the auto-tuned kernel send
/// buffer then hides seconds of backlog from a slow reader. Here the kernel side
/// is bounded by a fixed SO_SNDBUF and everything else waits in `pending`, so
/// "frame fully written" is a meaningful backpressure signal.
final class ClientSession {
    let address: String
    /// FRAME messages queued for this client and not yet fully written to the
    /// socket. Drives backpressure for clients that do not send FRAME_ACK.
    var framesUnsent = 0
    /// FRAMEs sent vs. acknowledged; limits frames in flight once the client acks.
    fileprivate(set) var ackWindow: FrameAckWindow
    /// The next encode for this client must be a keyframe.
    var needsKeyframe = true
    /// Last CONFIG sent on this connection.
    var sentConfig: ConfigMessage?
    fileprivate(set) var isActive = true

    fileprivate let fd: Int32
    fileprivate var reader = MessageReader(expectHello: false)
    fileprivate var pending: [(data: Data, written: Int, completion: (() -> Void)?)] = []
    fileprivate var readSource: DispatchSourceRead?
    fileprivate var writeSource: DispatchSourceWrite?
    fileprivate var writeSourceRunning = false

    fileprivate init(fd: Int32, address: String, maxFramesInFlight: UInt32) {
        self.fd = fd
        self.address = address
        ackWindow = FrameAckWindow(limit: maxFramesInFlight)
    }

    /// Bytes accepted by `send` but not yet handed to the kernel.
    var unsentBytes: Int { pending.reduce(0) { $0 + $1.data.count - $1.written } }
}

/// TCP server on 0.0.0.0: one active client, a new connection replaces the previous one.
final class MirrorServer {
    /// Kernel send buffer cap (unsent + unacknowledged) for clients that do not send
    /// FRAME_ACK: ~35 ms at 30 Mbit/s, the latency bound for them. Disables macOS
    /// send-buffer auto-tuning (up to 4 MiB).
    static let socketSendBuffer: Int32 = 128 * 1024
    /// Once a client acknowledges frames, the ack window bounds latency instead, and a
    /// larger buffer stops TCP from being window-limited on Wi-Fi: measured to a Quest 3
    /// on 5 GHz, 128 KiB caps throughput at ~120 Mbit/s, 512 KiB reaches ~285 Mbit/s
    /// (a 200 KB 4K keyframe goes out in ~6 ms instead of ~13 ms).
    static let ackedSocketSendBuffer: Int32 = 512 * 1024

    let port: UInt16
    let queue: DispatchQueue
    /// FRAME_ACK window: frames sent but not yet acknowledged, per client.
    let maxFramesInFlight: UInt32
    /// Called on `queue` right after a client connected and was sent hello (+ cached CONFIG).
    var onClientReady: ((ClientSession) -> Void)?
    /// Called on `queue` after a valid FRAME_ACK (the window may have opened).
    var onFrameAck: ((ClientSession) -> Void)?
    /// Most recent CONFIG produced by the encoder; sent right after the hello.
    var cachedConfig: ConfigMessage?
    /// Steps sent to every client right after the hello (--debug-control-on-connect).
    var controlOnConnect: [DebugControlStep] = []
    /// Curvature/dim; sent to every client right after the hello and on every change.
    private(set) var displaySettings = DisplaySettings.defaults
    /// Called on `queue` with the active client's address, or nil when none is connected.
    var onActiveClientChanged: ((String?) -> Void)?

    private(set) var activeClient: ClientSession?
    private(set) var bytesSent = 0
    private var listenFD: Int32 = -1
    private var acceptSource: DispatchSourceRead?
    private var readBuffer = [UInt8](repeating: 0, count: 64 * 1024)

    init(port: UInt16, maxFramesInFlight: UInt32, queue: DispatchQueue) {
        self.port = port
        self.maxFramesInFlight = maxFramesInFlight
        self.queue = queue
    }

    func start() throws {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { throw posixError("socket") }
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        addr.sin_addr = in_addr(s_addr: INADDR_ANY)
        let bound = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        guard bound == 0 else {
            let error = posixError("bind port \(port)")
            close(fd)
            throw error
        }
        guard listen(fd, 8) == 0 else {
            let error = posixError("listen")
            close(fd)
            throw error
        }
        _ = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)
        listenFD = fd
        let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
        source.setEventHandler { [weak self] in self?.acceptPending() }
        source.setCancelHandler { close(fd) }
        source.resume()
        acceptSource = source
        log("server: listening on 0.0.0.0:\(port)")
    }

    func stop() {
        acceptSource?.cancel()
        acceptSource = nil
        if let client = activeClient { drop(client, reason: "server shutting down") }
    }

    /// Queues a message for `client` if it is still the active one; `completion`
    /// runs on `queue` once the message is fully written to the socket.
    @discardableResult
    func send(_ message: Message, to client: ClientSession, completion: (() -> Void)? = nil) -> Bool {
        guard client.isActive else { return false }
        if case .frame = message { client.ackWindow.recordSent() }
        enqueue(message.encoded(), to: client, completion: completion)
        return true
    }

    /// Sends CONTROL to the active client right away (not subject to the frame windows;
    /// it only queues behind data already waiting for this socket). Returns the client's
    /// address, or nil if no client is connected.
    func sendControl(_ command: ControlCommand) -> String? {
        guard let client = activeClient, send(.control(command), to: client) else { return nil }
        return client.address
    }

    /// Records new settings and sends the changed value(s) to the active client.
    func updateDisplaySettings(_ settings: DisplaySettings) {
        let old = displaySettings
        displaySettings = settings
        guard let client = activeClient else { return }
        for (new, previous) in zip(settings.commands, old.commands) where new != previous {
            send(.control(new), to: client)
        }
    }

    // MARK: Connections

    private func acceptPending() {
        while true {
            var addr = sockaddr_storage()
            var len = socklen_t(MemoryLayout<sockaddr_storage>.size)
            let fd = withUnsafeMutablePointer(to: &addr) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { accept(listenFD, $0, &len) }
            }
            if fd < 0 {
                if errno != EWOULDBLOCK && errno != EINTR { log("server: accept failed: \(String(cString: strerror(errno)))") }
                return
            }
            configure(fd)
            let client = ClientSession(fd: fd, address: describe(addr), maxFramesInFlight: maxFramesInFlight)
            if let previous = activeClient {
                drop(previous, reason: "replaced by \(client.address)")
            }
            activeClient = client
            log("server: client \(client.address) connected")
            onActiveClientChanged?(client.address)
            startIO(client)
            enqueue(WireProtocol.helloBytes(), to: client, completion: nil)
            if let config = cachedConfig {
                send(.config(config), to: client)
                client.sentConfig = config
            }
            // Absolute state the sender owns; clients apply it whenever it arrives.
            for command in displaySettings.commands { send(.control(command), to: client) }
            if !controlOnConnect.isEmpty { sendControlOnConnect(to: client) }
            onClientReady?(client)
        }
    }

    /// --debug-control-on-connect: the first command goes out right after the hello, the
    /// rest one per 1/60 s (like a 60 Hz mouse gesture), `wait:MS` steps add a pause;
    /// only while the client stays connected.
    private func sendControlOnConnect(to client: ClientSession) {
        var schedule: [(offsetMicros: Int, command: ControlCommand)] = []
        var offset = 0
        for step in controlOnConnect {
            switch step {
            case .send(let command):
                schedule.append((offset, command))
                offset += 16_667
            case .wait(let milliseconds):
                offset += milliseconds * 1000
            }
        }
        log("server: sending \(schedule.count) CONTROL command(s) to \(client.address) over "
            + String(format: "%.2f s (--debug-control-on-connect)", Double(offset) / 1e6))
        for (index, item) in schedule.enumerated() {
            queue.asyncAfter(deadline: .now() + .microseconds(item.offsetMicros)) { [weak self, weak client] in
                guard let self, let client, client.isActive else { return }
                self.send(.control(item.command), to: client)
                if schedule.count <= 20 || index == schedule.count - 1 {
                    log("server: sent CONTROL \(item.command) to \(client.address) (\(index + 1)/\(schedule.count))")
                }
            }
        }
    }

    private func configure(_ fd: Int32) {
        _ = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)
        var one: Int32 = 1
        var sendBuffer = Self.socketSendBuffer
        let size = socklen_t(MemoryLayout<Int32>.size)
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, size)
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, size)
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sendBuffer, size)
    }

    private func startIO(_ client: ClientSession) {
        let fd = client.fd
        let read = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
        let write = DispatchSource.makeWriteSource(fileDescriptor: fd, queue: queue)
        // Close the descriptor only after both sources are cancelled.
        var remaining = 2
        let closeWhenDone = {
            remaining -= 1
            if remaining == 0 { close(fd) }
        }
        read.setEventHandler { [weak self, weak client] in
            guard let self, let client else { return }
            self.readAvailable(client)
        }
        read.setCancelHandler(handler: closeWhenDone)
        write.setEventHandler { [weak self, weak client] in
            guard let self, let client else { return }
            self.flush(client)
        }
        write.setCancelHandler(handler: closeWhenDone)
        client.readSource = read
        client.writeSource = write
        read.resume()
    }

    private func drop(_ client: ClientSession, reason: String) {
        guard client.isActive else { return }
        client.isActive = false
        if activeClient === client {
            activeClient = nil
            onActiveClientChanged?(nil)
        }
        log("server: client \(client.address) disconnected (\(reason))")
        client.pending.removeAll()
        client.readSource?.cancel()
        if let write = client.writeSource {
            if !client.writeSourceRunning { write.resume() } // a suspended source never cancels
            write.cancel()
        }
    }

    // MARK: Writing

    private func enqueue(_ data: Data, to client: ClientSession, completion: (() -> Void)?) {
        bytesSent += data.count
        client.pending.append((data, 0, completion))
        flush(client)
    }

    /// Writes as much as the socket accepts; the write source resumes the rest.
    private func flush(_ client: ClientSession) {
        guard client.isActive else { return }
        while let first = client.pending.first {
            let n = first.data.withUnsafeBytes { raw in
                Darwin.write(client.fd, raw.baseAddress! + first.written, raw.count - first.written)
            }
            if n < 0 {
                if errno == EINTR { continue }
                if errno == EAGAIN { break }
                drop(client, reason: "write error: \(String(cString: strerror(errno)))")
                return
            }
            client.pending[0].written += n
            if client.pending[0].written == first.data.count {
                client.pending.removeFirst()
                if let completion = first.completion { queue.async(execute: completion) }
            }
        }
        let wantWritable = !client.pending.isEmpty
        if wantWritable != client.writeSourceRunning, let write = client.writeSource {
            client.writeSourceRunning = wantWritable
            if wantWritable { write.resume() } else { write.suspend() }
        }
    }

    // MARK: Reading

    private func readAvailable(_ client: ClientSession) {
        guard client.isActive else { return }
        let n = readBuffer.withUnsafeMutableBytes { Darwin.read(client.fd, $0.baseAddress!, $0.count) }
        if n == 0 {
            return drop(client, reason: "closed by peer")
        }
        if n < 0 {
            if errno == EAGAIN || errno == EINTR { return }
            return drop(client, reason: "read error: \(String(cString: strerror(errno)))")
        }
        client.reader.append(Data(readBuffer[0..<n]))
        do {
            while let message = try client.reader.next() {
                handle(message, from: client)
            }
        } catch {
            drop(client, reason: "protocol error: \(error)")
        }
    }

    private func handle(_ message: Message, from client: ClientSession) {
        switch message {
        case .ping(let clientTime):
            send(.pong(clientTimeMicros: clientTime, serverTimeMicros: HostClock.nowMicros()), to: client)
        case .frameAck(let consumed):
            let wasActive = client.ackWindow.isActive
            guard client.ackWindow.recordAck(framesConsumed: consumed) else {
                log("server: client \(client.address) sent invalid FRAME_ACK \(consumed) "
                    + "(acked \(client.ackWindow.framesAcked), sent \(client.ackWindow.framesSent)); ignored")
                return
            }
            if !wasActive {
                var size = Self.ackedSocketSendBuffer
                setsockopt(client.fd, SOL_SOCKET, SO_SNDBUF, &size, socklen_t(MemoryLayout<Int32>.size))
                log("server: client \(client.address) acknowledges frames; at most \(maxFramesInFlight) in flight, "
                    + "send buffer \(size / 1024) KiB")
            }
            onFrameAck?(client)
        case .unknown:
            break // Forward compatibility: skip unknown types.
        case .config, .frame, .control, .pong:
            break // Server-to-client types; ignore if a client echoes them.
        }
    }
}

private func describe(_ storage: sockaddr_storage) -> String {
    var storage = storage
    let length = socklen_t(storage.ss_len)
    var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
    var service = [CChar](repeating: 0, count: Int(NI_MAXSERV))
    let hostCount = socklen_t(host.count), serviceCount = socklen_t(service.count)
    let status = withUnsafePointer(to: &storage) {
        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
            getnameinfo($0, length, &host, hostCount, &service, serviceCount, NI_NUMERICHOST | NI_NUMERICSERV)
        }
    }
    return status == 0 ? "\(String(cString: host)):\(String(cString: service))" : "unknown"
}

private func posixError(_ what: String) -> Error {
    OptionError("\(what) failed: \(String(cString: strerror(errno)))")
}
