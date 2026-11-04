import Darwin
import Foundation
import MirrorProtocol

/// Finds a sender on the LAN (docs/protocol.md, "Discovery"): broadcasts DISCOVER to
/// 255.255.255.255 and each interface's directed broadcast address once per second
/// until the first valid ANNOUNCE arrives. Use from `queue` only.
final class Discoverer {
    let port: UInt16
    private let queue: DispatchQueue
    private var fd: Int32 = -1
    private var readSource: DispatchSourceRead?
    private var timer: DispatchSourceTimer?
    private var completion: ((String, UInt16, String) -> Void)?
    private var reportedErrors = Set<String>()
    private var buffer = [UInt8](repeating: 0, count: 2048)

    init(port: UInt16, queue: DispatchQueue) {
        self.port = port
        self.queue = queue
    }

    /// Calls `completion(host, tcpPort, name)` on `queue` for the first ANNOUNCE.
    func discover(completion: @escaping (String, UInt16, String) -> Void) {
        stop()
        self.completion = completion
        let fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else {
            log("discovery: socket failed: \(String(cString: strerror(errno)))")
            return retryLater()
        }
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, socklen_t(MemoryLayout<Int32>.size))
        _ = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)
        self.fd = fd
        let read = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
        read.setEventHandler { [weak self] in self?.readAnnounces() }
        read.setCancelHandler { close(fd) }
        read.resume()
        readSource = read

        let targets = Self.broadcastAddresses()
        log("discovery: broadcasting DISCOVER every 1 s to \(targets.joined(separator: ", ")) UDP \(port)")
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now(), repeating: 1)
        timer.setEventHandler { [weak self] in self?.sendDiscover(to: targets) }
        timer.resume()
        self.timer = timer
    }

    private func stop() {
        timer?.cancel()
        timer = nil
        readSource?.cancel()
        readSource = nil
        fd = -1
    }

    private func retryLater() {
        queue.asyncAfter(deadline: .now() + 1) { [weak self] in
            guard let self, let completion = self.completion else { return }
            self.discover(completion: completion)
        }
    }

    private func sendDiscover(to targets: [String]) {
        let datagram = DiscoveryMessage.discover.encoded()
        for target in targets {
            var addr = sockaddr_in()
            addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
            addr.sin_family = sa_family_t(AF_INET)
            addr.sin_port = port.bigEndian
            inet_pton(AF_INET, target, &addr.sin_addr)
            let sent = datagram.withUnsafeBytes { raw in
                withUnsafePointer(to: &addr) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                        sendto(fd, raw.baseAddress!, raw.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                    }
                }
            }
            if sent < 0 {
                let error = "\(target): \(String(cString: strerror(errno)))"
                if reportedErrors.insert(error).inserted { log("discovery: send to \(error)") }
            }
        }
    }

    private func readAnnounces() {
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
            if n < 0 { return }
            guard case .announce(let tcpPort, let name)? = DiscoveryMessage.decode(Data(buffer[0..<n])),
                  let completion else { continue }
            let host = String(cString: inet_ntoa(from.sin_addr))
            stop()
            self.completion = nil
            completion(host, tcpPort, name)
            return
        }
    }

    /// 255.255.255.255 plus the directed broadcast address of every up IPv4
    /// interface that supports broadcast (e.g. 192.168.1.255 on Wi-Fi).
    static func broadcastAddresses() -> [String] {
        var result = ["255.255.255.255"]
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0 else { return result }
        defer { freeifaddrs(list) }
        var cursor = list
        while let ifa = cursor?.pointee {
            defer { cursor = ifa.ifa_next }
            let flags = Int32(ifa.ifa_flags)
            guard flags & IFF_UP != 0, flags & IFF_BROADCAST != 0, flags & IFF_LOOPBACK == 0,
                  let addr = ifa.ifa_addr, addr.pointee.sa_family == sa_family_t(AF_INET),
                  let broadcast = ifa.ifa_dstaddr else { continue } // ifa_broadaddr on BSD
            let text = broadcast.withMemoryRebound(to: sockaddr_in.self, capacity: 1) {
                String(cString: inet_ntoa($0.pointee.sin_addr))
            }
            if !result.contains(text) { result.append(text) }
        }
        return result
    }
}
