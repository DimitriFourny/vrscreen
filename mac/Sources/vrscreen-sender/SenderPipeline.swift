import CoreVideo
import Foundation
import MirrorProtocol

/// Capture → encode → send, with backpressure that never drops encoded frames:
/// while the client's window is full (unsent socket backlog, or for clients that
/// send FRAME_ACK, frames not yet consumed) or the encoder is busy, new captures
/// are not encoded; only the latest capture is kept and is encoded as soon as
/// there is room, so the final state of a now-static screen is never lost.
/// All methods must be called on `queue`.
final class SenderPipeline {
    /// Encode only while fewer frames than this are encoded but not yet fully
    /// written to the client's socket.
    static let maxUnsentFrames = 2
    /// One frame in the encoder at a time: the low-latency (rtvc) encoder encodes
    /// synchronously inside VTCompressionSessionEncodeFrame (~15 ms at 4K), so a
    /// second queued frame would only wait. Newer captures replace the pending one.
    static let maxEncodingFrames = 1

    let queue: DispatchQueue
    let server: MirrorServer
    /// Owned by `encoderQueue`; never touched from `queue`.
    private let encoder: VideoEncoder
    private let encoderQueue = DispatchQueue(label: "vrscreen.encoder", qos: .userInteractive)

    private struct Capture {
        var buffer: CVPixelBuffer
        var captureMicros: UInt64
        var sequence: UInt64
    }

    private var latest: Capture?
    private var captureSequence: UInt64 = 0
    private var lastEncodedSequence: UInt64 = 0
    /// Frames submitted to the encoder whose output has not arrived yet.
    private var framesEncoding = 0

    private var stats = Stats()
    private var totalSkipped = 0
    private var lastStatsTime = Date()
    private var lastBytesSent = 0

    private struct Stats {
        var captured = 0
        var encoded = 0
        /// Captures replaced by a newer one before they could be encoded.
        var skipped = 0
        var keyframes = 0
        /// Highest FRAME_ACK in-flight count right after a send.
        var maxInFlight: UInt32 = 0
    }

    init(queue: DispatchQueue, encoder: VideoEncoder, server: MirrorServer) {
        self.queue = queue
        self.encoder = encoder
        self.server = server
        server.onClientReady = { [weak self] client in
            client.needsKeyframe = true
            self?.pump()
        }
        server.onFrameAck = { [weak self] _ in self?.pump() }
    }

    /// A new captured image (only frames that carry an image).
    func handleCapture(_ buffer: CVPixelBuffer, captureMicros: UInt64) {
        if server.activeClient != nil, let previous = latest, previous.sequence > lastEncodedSequence {
            stats.skipped += 1
        }
        captureSequence += 1
        latest = Capture(buffer: buffer, captureMicros: captureMicros, sequence: captureSequence)
        // Session setup takes ~100-300 ms; do it before a client needs it. A failure
        // here resurfaces (and is logged) on the next encode.
        encoderQueue.async { [encoder] in try? encoder.prepare(for: buffer) }
        stats.captured += 1
        pump()
    }

    private func pump() {
        guard let client = server.activeClient, let latest else { return }
        guard latest.sequence > lastEncodedSequence || client.needsKeyframe else { return }
        // Clients that send FRAME_ACK are limited by frames in flight (sent, not yet
        // consumed); others only by the socket backlog. The first frame for a new
        // client always fits: its window starts empty.
        guard client.framesUnsent < Self.maxUnsentFrames, client.ackWindow.isOpen,
              framesEncoding < Self.maxEncodingFrames else { return }
        let forceKeyframe = client.needsKeyframe
        client.needsKeyframe = false
        lastEncodedSequence = latest.sequence
        framesEncoding += 1
        let capture = latest
        encoderQueue.async { [encoder, weak self] in
            encoder.encode(capture.buffer, captureMicros: capture.captureMicros, forceKeyframe: forceKeyframe) { result in
                self?.queue.async { self?.handleEncoded(result, for: client) }
            }
        }
    }

    /// Flushes and tears down the encoder (blocks until pending frames finished).
    func stop() {
        encoderQueue.sync { encoder.invalidate() }
    }

    private func handleEncoded(_ result: Result<VideoEncoder.Output?, Error>, for client: ClientSession) {
        framesEncoding -= 1
        switch result {
        case .failure(let error):
            log("encoder: \(error)")
            client.needsKeyframe = true
        case .success(nil):
            break // Dropped by the encoder itself; its reference chain stays valid.
        case .success(let output?):
            stats.encoded += 1
            if output.frame.isKeyframe { stats.keyframes += 1 }
            server.cachedConfig = output.config
            guard client.isActive else { break } // Output for a replaced client.
            if client.sentConfig != output.config {
                if client.sentConfig != nil {
                    log("server: stream format changed to \(output.config.codec) \(output.config.width)x\(output.config.height), sending CONFIG")
                }
                server.send(.config(output.config), to: client)
                client.sentConfig = output.config
            }
            client.framesUnsent += 1
            server.send(.frame(output.frame), to: client) { [weak self] in
                client.framesUnsent -= 1
                self?.pump()
            }
            stats.maxInFlight = max(stats.maxInFlight, client.ackWindow.framesInFlight)
        }
        pump()
    }

    /// Logs once-per-second stats. Call from a 1 s timer on `queue`.
    func logStats() {
        let now = Date()
        let dt = now.timeIntervalSince(lastStatsTime)
        lastStatsTime = now
        let bytes = server.bytesSent - lastBytesSent
        lastBytesSent = server.bytesSent
        totalSkipped += stats.skipped
        let clientText: String
        if let c = server.activeClient {
            var text = "client \(c.address) | backlog \(c.framesUnsent) frames \(c.unsentBytes / 1024) KiB"
            if c.ackWindow.isActive {
                text += " | in flight \(c.ackWindow.framesInFlight)/\(c.ackWindow.limit) (max \(stats.maxInFlight))"
            }
            clientText = text
        } else {
            clientText = "no client"
        }
        log(String(format: "capture %.1f fps | encoded %.1f fps (%d key) | skipped %d (total %d) | %.2f Mbit/s | %@",
                   Double(stats.captured) / dt, Double(stats.encoded) / dt, stats.keyframes,
                   stats.skipped, totalSkipped, Double(bytes) * 8 / dt / 1e6, clientText))
        stats = Stats()
    }
}
