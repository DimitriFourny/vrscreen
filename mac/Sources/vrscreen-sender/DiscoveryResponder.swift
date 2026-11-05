import Darwin
import Foundation
import MirrorProtocol
import SystemConfiguration

/// Answers DISCOVER datagrams on UDP 0.0.0.0:<port> with a unicast ANNOUNCE that
/// tells the client which TCP port to connect to (docs/protocol.md, "Discovery").
final class DiscoveryResponder {
    let port: UInt16
    private let announce: Data
    private let name: String
    private let queue: DispatchQueue
    /// Called on `queue` after each ANNOUNCE: nil when it went out, else the errno. macOS 15+
    /// Local Network privacy makes sends to the LAN fail with EHOSTUNREACH when denied.
    var onAnnounceResult: ((Int32?) -> Void)?
    private var fd: Int32 = -1
    private var source: DispatchSourceRead?
    private var buffer = [UInt8](repeating: 0, count: 2048)

    init(port: UInt16, tcpPort: UInt16, queue: DispatchQueue) {
        self.port = port
        self.queue = queue
        name = Self.macName()
        announce = DiscoveryMessage.announce(tcpPort: tcpPort, name: name).encoded()
    }

    func start() throws {
        let fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else { throw SocketError(what: "discovery socket", port: port) }
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
            let error = SocketError(what: "discovery: bind UDP port \(port)", port: port)
            close(fd)
            throw error
        }
        _ = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)
        self.fd = fd
        let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
        source.setEventHandler { [weak self] in self?.readDatagrams() }
        source.setCancelHandler { close(fd) }
        source.resume()
        self.source = source
        log("discovery: answering on UDP 0.0.0.0:\(port) as \"\(name)\"")
    }

    var isRunning: Bool { source != nil }

    func stop() {
        source?.cancel()
        source = nil
    }

    private func readDatagrams() {
        while true {
            var from = sockaddr_in()
            var fromLength = socklen_t(MemoryLayout<sockaddr_in>.size)
            let n = buffer.withUnsafeMutableBytes { raw in
                withUnsafeMutablePointer(to: &from) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                        recvfrom(fd, raw.baseAddress!, raw.count, 0, $0, &fromLength)
                    }
                }
            }
            if n < 0 { return } // EAGAIN: drained
            let sourceText = "\(String(cString: inet_ntoa(from.sin_addr))):\(UInt16(bigEndian: from.sin_port))"
            guard DiscoveryMessage.decode(Data(buffer[0..<n])) == .discover else {
                log("discovery: ignored \(n)-byte datagram from \(sourceText)")
                continue
            }
            let sent = announce.withUnsafeBytes { raw in
                withUnsafePointer(to: &from) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                        sendto(fd, raw.baseAddress!, raw.count, 0, $0, fromLength)
                    }
                }
            }
            if sent < 0 {
                let code = errno
                log("discovery: DISCOVER from \(sourceText), ANNOUNCE failed: \(String(cString: strerror(code)))")
                onAnnounceResult?(code)
            } else {
                log("discovery: DISCOVER from \(sourceText), announced")
                onAnnounceResult?(nil)
            }
        }
    }

    /// The Mac's computer name ("Df0's MacBook Pro"), else its local host name.
    private static func macName() -> String {
        if let name = SCDynamicStoreCopyComputerName(nil, nil) as String?, !name.isEmpty { return name }
        if let name = SCDynamicStoreCopyLocalHostName(nil) as String?, !name.isEmpty { return name }
        return ProcessInfo.processInfo.hostName
    }
}
