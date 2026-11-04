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
    /// socket. Drives backpressure.
    var framesUnsent = 0
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

    fileprivate init(fd: Int32, address: String) {
        self.fd = fd
        self.address = address
    }

    /// Bytes accepted by `send` but not yet handed to the kernel.
    var unsentBytes: Int { pending.reduce(0) { $0 + $1.data.count - $1.written } }
}

/// TCP server on 0.0.0.0: one active client, a new connection replaces the previous one.
final class MirrorServer {
    /// Kernel send buffer cap (unsent + unacknowledged): ~35 ms at 30 Mbit/s, still
    /// above the bandwidth-delay product of USB or a local Wi-Fi link (30 Mbit/s at
    /// up to ~35 ms RTT). Disables macOS send-buffer auto-tuning (up to 4 MiB).
    static let socketSendBuffer: Int32 = 128 * 1024

    let port: UInt16
    let queue: DispatchQueue
    /// Called on `queue` right after a client connected and was sent hello (+ cached CONFIG).
    var onClientReady: ((ClientSession) -> Void)?
    /// Most recent CONFIG produced by the encoder; sent right after the hello.
    var cachedConfig: ConfigMessage?

    private(set) var activeClient: ClientSession?
    private(set) var bytesSent = 0
    private var listenFD: Int32 = -1
    private var acceptSource: DispatchSourceRead?
    private var readBuffer = [UInt8](repeating: 0, count: 64 * 1024)

    init(port: UInt16, queue: DispatchQueue) {
        self.port = port
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
        enqueue(message.encoded(), to: client, completion: completion)
        return true
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
            let client = ClientSession(fd: fd, address: describe(addr))
            if let previous = activeClient {
                drop(previous, reason: "replaced by \(client.address)")
            }
            activeClient = client
            log("server: client \(client.address) connected")
            startIO(client)
            enqueue(WireProtocol.helloBytes(), to: client, completion: nil)
            if let config = cachedConfig {
                send(.config(config), to: client)
                client.sentConfig = config
            }
            onClientReady?(client)
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
        if activeClient === client { activeClient = nil }
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
        case .unknown:
            break // Forward compatibility: skip unknown types.
        case .config, .frame, .pong:
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
