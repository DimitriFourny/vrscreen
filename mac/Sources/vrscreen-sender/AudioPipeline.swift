import CoreMedia
import Foundation
import MirrorProtocol

/// Audio → headset: source buffers (ScreenCaptureKit system audio, or the test source's
/// beeps) are converted to s16le, re-chunked to 10 ms and handed to the server, which sends
/// them to the current client outside the video frame windows. Runs on its own `queue`, so
/// it never waits for (or delays) video encoding.
final class AudioPipeline {
    static let config = AudioConfigMessage.standard
    static let framesPerChunk = Int(config.sampleRate) / 100

    let queue = DispatchQueue(label: "vrscreen.audio", qos: .userInteractive)
    private let server: MirrorServer
    private let serverQueue: DispatchQueue
    /// Owned by `queue`.
    private var chunker = AudioChunker(sampleRate: Int(config.sampleRate), channels: Int(config.channels),
                                       framesPerChunk: framesPerChunk)
    private var loggedFormat = false
    private var unsupportedFormatLogged = false

    init(server: MirrorServer, serverQueue: DispatchQueue) {
        self.server = server
        self.serverQueue = serverQueue
    }

    /// Drops the pending partial chunk (audio switched off). Any thread.
    func reset() {
        queue.async { self.chunker.reset() }
    }

    /// Partial chunks discarded at discontinuities so far. On `queue`.
    var discardedFrames: Int { chunker.discardedFrames }

    /// One ScreenCaptureKit audio buffer (Float32 or Int16, planar or interleaved, 48 kHz,
    /// 1 or 2 channels). On `queue`.
    func handle(_ sampleBuffer: CMSampleBuffer) {
        guard sampleBuffer.isValid, let description = sampleBuffer.formatDescription,
              let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(description)?.pointee else { return }
        let isFloat = asbd.mFormatFlags & kAudioFormatFlagIsFloat != 0
        let planar = asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved != 0
        let channels = Int(asbd.mChannelsPerFrame)
        guard asbd.mFormatID == kAudioFormatLinearPCM, Int(asbd.mSampleRate) == chunker.sampleRate,
              (isFloat && asbd.mBitsPerChannel == 32) || (!isFloat && asbd.mBitsPerChannel == 16),
              channels == 1 || channels == chunker.channels else {
            if !unsupportedFormatLogged {
                unsupportedFormatLogged = true
                log("audio: unsupported capture format \(asbd.mSampleRate) Hz, \(channels) ch, "
                    + "\(asbd.mBitsPerChannel) bit, flags 0x\(String(asbd.mFormatFlags, radix: 16)); audio not sent")
            }
            return
        }
        if !loggedFormat {
            loggedFormat = true
            let lag = Double(Int64(HostClock.nowMicros()) - Int64(HostClock.micros(sampleBuffer.presentationTimeStamp))) / 1000
            log("audio: capturing system audio, \(Int(asbd.mSampleRate)) Hz, \(channels) ch, "
                + "\(isFloat ? "float32" : "s16")\(planar ? " planar" : ""), "
                + "\(sampleBuffer.numSamples) frames per buffer (first buffer \(String(format: "%.1f", lag)) ms old) "
                + "→ s16le \(chunker.channels) ch, \(chunker.framesPerChunk)-frame chunks")
        }
        let frames = sampleBuffer.numSamples
        let micros = HostClock.micros(sampleBuffer.presentationTimeStamp)
        try? sampleBuffer.withAudioBufferList { list, _ in
            // Mono is duplicated to every channel; planar has one buffer per channel.
            let buffers = Array(list)
            func plane(_ channel: Int) -> UnsafeMutableRawPointer? {
                buffers[planar ? min(channel, buffers.count - 1) : 0].mData
            }
            let stride = planar ? 1 : channels
            let chunks: [AudioMessage]
            if isFloat {
                let planes = (0..<chunker.channels).map { plane($0)?.assumingMemoryBound(to: Float.self) }
                guard planes.allSatisfy({ $0 != nil }) else { return }
                chunks = chunker.append(frameCount: frames, captureMicros: micros) { frame, channel in
                    let source = planar ? 0 : min(channel, channels - 1)
                    return planes[channel]![frame * stride + source]
                }
            } else {
                let planes = (0..<chunker.channels).map { plane($0)?.assumingMemoryBound(to: Int16.self) }
                guard planes.allSatisfy({ $0 != nil }) else { return }
                chunks = chunker.append(frameCount: frames, captureMicros: micros) { frame, channel in
                    let source = planar ? 0 : min(channel, channels - 1)
                    return Float(planes[channel]![frame * stride + source]) / 32767
                }
            }
            send(chunks)
        }
    }

    /// Interleaved float samples (`chunker.channels` per frame), the first captured at
    /// `captureMicros`. On `queue`.
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
