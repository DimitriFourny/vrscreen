import CoreAudio
import CoreMedia
import Foundation
import MirrorProtocol

/// Audio → headset: source buffers (the Core Audio process tap, ScreenCaptureKit system
/// audio, or the test source's beeps) are converted to s16le, re-chunked to 10 ms and
/// handed to the server, which sends them to the current client outside the video frame
/// windows. Runs on its own `queue`, so it never waits for (or delays) video encoding.
///
/// Captured system audio is stamped with the time the Mac would play it: capture time plus
/// the default output device's presentation latency (`OutputLatency`). Video players send
/// their audio that much ahead of the matching picture (160 ms for AirPods), so this keeps
/// AUDIO in sync with FRAME capture times.
final class AudioPipeline {
    static let config = AudioConfigMessage.standard
    static let framesPerChunk = Int(config.sampleRate) / 100

    /// Where captured audio comes from (log and stats labels).
    enum Source: String {
        case tap = "process tap"
        case sck = "ScreenCaptureKit"
        case test = "test pattern"

        var statsLabel: String { self == .tap ? "tap" : "SCK" }
    }

    let queue = DispatchQueue(label: "vrscreen.audio", qos: .userInteractive)
    private let server: MirrorServer
    private let serverQueue: DispatchQueue
    /// Owned by `queue`.
    private var chunker = AudioChunker(sampleRate: Int(config.sampleRate), channels: Int(config.channels),
                                       framesPerChunk: framesPerChunk)
    private var loggedSources: Set<String> = []
    private var unsupportedFormatLogged = false
    /// How late each captured buffer reached the pipeline since the last `takeCaptureStats`:
    /// now − capture time of the buffer's last sample, ms. On `queue`.
    private var deliveryAgesMs: [Double] = []
    private var deliverySource: Source?
    /// Added to captured (not synthesized) audio timestamps. On `queue`.
    private var presentationLatencyMicros: UInt64 = 0
    private var outputLatency: OutputLatency?

    init(server: MirrorServer, serverQueue: DispatchQueue) {
        self.server = server
        self.serverQueue = serverQueue
    }

    /// Follows the default output device's presentation latency (captured audio only).
    func trackOutputLatency() {
        outputLatency = OutputLatency(queue: queue) { [weak self] latency in
            guard let self else { return }
            self.presentationLatencyMicros = latency.micros
            log("audio: Mac output \"\(latency.deviceName)\" plays \(String(format: "%.1f", Double(latency.micros) / 1000)) ms "
                + "after capture; captured audio is stamped with that playback time")
        }
    }

    /// "tap delivery p50 0 max 0 ms" since the last call; nil without captured buffers. On `queue`.
    func takeCaptureStats() -> String? {
        defer { deliveryAgesMs.removeAll(keepingCapacity: true) }
        guard !deliveryAgesMs.isEmpty, let deliverySource else { return nil }
        let ages = deliveryAgesMs.sorted()
        return String(format: "%@ delivery p50 %.1f max %.1f ms", deliverySource.statsLabel,
                      max(0, ages[ages.count / 2]), max(0, ages.last!))
    }

    /// Drops the pending partial chunk (audio switched off). Any thread.
    func reset() {
        queue.async { self.chunker.reset() }
    }

    /// One ScreenCaptureKit audio buffer. On `queue`.
    func handle(_ sampleBuffer: CMSampleBuffer) {
        guard sampleBuffer.isValid, let description = sampleBuffer.formatDescription,
              let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(description)?.pointee else { return }
        let micros = HostClock.micros(sampleBuffer.presentationTimeStamp)
        let frames = sampleBuffer.numSamples
        try? sampleBuffer.withAudioBufferList { list, _ in
            handle(list, format: asbd, frameCount: frames, captureMicros: micros, source: .sck)
        }
    }

    /// Captured linear PCM: Float32 or Int16, planar or interleaved, 48 kHz, 1 or 2 channels;
    /// the first frame captured at `captureMicros` (host clock). On `queue`.
    func handle(_ list: UnsafeMutableAudioBufferListPointer, format asbd: AudioStreamBasicDescription,
                frameCount frames: Int, captureMicros micros: UInt64, source: Source) {
        let isFloat = asbd.mFormatFlags & kAudioFormatFlagIsFloat != 0
        let planar = asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved != 0
        let channels = Int(asbd.mChannelsPerFrame)
        guard asbd.mFormatID == kAudioFormatLinearPCM, Int(asbd.mSampleRate) == chunker.sampleRate,
              (isFloat && asbd.mBitsPerChannel == 32) || (!isFloat && asbd.mBitsPerChannel == 16),
              channels == 1 || channels == chunker.channels, frames > 0 else {
            if !unsupportedFormatLogged {
                unsupportedFormatLogged = true
                log("audio: unsupported \(source.rawValue) format \(asbd.mSampleRate) Hz, \(channels) ch, "
                    + "\(asbd.mBitsPerChannel) bit, flags 0x\(String(asbd.mFormatFlags, radix: 16)); audio not sent")
            }
            return
        }
        let endMicros = Double(micros) + Double(frames) * 1e6 / Double(chunker.sampleRate)
        deliveryAgesMs.append((Double(HostClock.nowMicros()) - endMicros) / 1000)
        deliverySource = source
        if loggedSources.insert(source.rawValue).inserted {
            log("audio: capturing system audio (\(source.rawValue)), \(Int(asbd.mSampleRate)) Hz, \(channels) ch, "
                + "\(isFloat ? "float32" : "s16")\(planar ? " planar" : ""), \(frames) frames per buffer "
                + "→ s16le \(chunker.channels) ch, \(chunker.framesPerChunk)-frame chunks")
        }
        // Mono is duplicated to every channel; planar has one buffer per channel.
        func plane(_ channel: Int) -> UnsafeMutableRawPointer? {
            guard list.count > 0 else { return nil }
            return list[planar ? min(channel, list.count - 1) : 0].mData
        }
        let stride = planar ? 1 : channels
        let stamped = micros + presentationLatencyMicros
        let chunks: [AudioMessage]
        if isFloat {
            let planes = (0..<chunker.channels).map { plane($0)?.assumingMemoryBound(to: Float.self) }
            guard planes.allSatisfy({ $0 != nil }) else { return }
            chunks = chunker.append(frameCount: frames, captureMicros: stamped) { frame, channel in
                let source = planar ? 0 : min(channel, channels - 1)
                return planes[channel]![frame * stride + source]
            }
        } else {
            let planes = (0..<chunker.channels).map { plane($0)?.assumingMemoryBound(to: Int16.self) }
            guard planes.allSatisfy({ $0 != nil }) else { return }
            chunks = chunker.append(frameCount: frames, captureMicros: stamped) { frame, channel in
                let source = planar ? 0 : min(channel, channels - 1)
                return Float(planes[channel]![frame * stride + source]) / 32767
            }
        }
        send(chunks)
    }

    /// Synthesized interleaved float samples (`chunker.channels` per frame), the first one
    /// at `captureMicros`, already a playback time. On `queue`.
    func handle(interleaved samples: [Float], captureMicros: UInt64) {
        let channels = chunker.channels
        let chunks = chunker.append(frameCount: samples.count / channels, captureMicros: captureMicros) { frame, channel in
            samples[frame * channels + channel]
        }
        send(chunks)
    }

    private func send(_ chunks: [AudioMessage]) {
        guard !chunks.isEmpty else { return }
        serverQueue.async { [server] in
            for chunk in chunks { server.sendAudio(chunk) }
        }
    }
}
