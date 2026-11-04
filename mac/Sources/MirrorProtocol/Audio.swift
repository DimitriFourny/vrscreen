import Foundation

/// `0x04 AUDIO_CONFIG`: `format u8 | sampleRate u32 | channels u8`.
public struct AudioConfigMessage: Equatable, Sendable {
    /// `format` value: PCM, signed 16-bit little-endian samples, channels interleaved.
    public static let pcmS16LE: UInt8 = 1
    /// What the Mac sender sends: 48 kHz stereo s16le.
    public static let standard = AudioConfigMessage(format: pcmS16LE, sampleRate: 48_000, channels: 2)

    /// Raw value so that receivers can ignore formats they do not know (see `docs/protocol.md`).
    public var format: UInt8
    public var sampleRate: UInt32
    public var channels: UInt8

    public init(format: UInt8, sampleRate: UInt32, channels: UInt8) {
        self.format = format
        self.sampleRate = sampleRate
        self.channels = channels
    }

    /// Bytes per frame (one sample of every channel) for `pcmS16LE`.
    public var bytesPerFrame: Int { Int(channels) * 2 }
}

/// `0x05 AUDIO`: `captureTimeMicros u64 | PCM bytes`. The PCM payload is in the format of
/// the last AUDIO_CONFIG (s16 **little-endian**, interleaved), unlike the big-endian header.
public struct AudioMessage: Equatable, Sendable {
    /// Sender host clock (same clock as FRAME `captureTimeMicros`), time of the first sample.
    public var captureTimeMicros: UInt64
    public var pcm: Data

    public init(captureTimeMicros: UInt64, pcm: Data) {
        self.captureTimeMicros = captureTimeMicros
        self.pcm = pcm
    }
}

public enum PCM16 {
    /// Float sample (nominal −1...1) → signed 16-bit: scaled by 32767, rounded, clamped.
    @inline(__always)
    public static func sample(_ value: Float) -> Int16 {
        let scaled = (value * 32767).rounded()
        if scaled >= 32767 { return .max }
        if scaled <= -32768 { return .min }
        if scaled.isNaN { return 0 }
        return Int16(scaled)
    }
}

/// Re-chunks a stream of audio buffers of any size into fixed chunks (10 ms at 48 kHz by
/// default) of s16le interleaved PCM, each stamped with the capture time of its first
/// sample. Input buffers carry the capture time of their first frame:
///
/// - A buffer that continues the previous one (within `gapToleranceMicros`) extends the
///   pending samples; the pending partial chunk is re-timed so the new buffer's first
///   frame lands exactly on its timestamp, so chunk times follow the source clock.
/// - A buffer further away (a gap, or time going backwards) discards the pending partial
///   chunk (counted in `discardedFrames`) and starts over at the new timestamp.
public struct AudioChunker {
    public let sampleRate: Int
    public let channels: Int
    public let framesPerChunk: Int
    public let gapToleranceMicros: Double
    /// Frames thrown away at discontinuities (partial chunks), since creation.
    public private(set) var discardedFrames = 0

    /// Interleaved samples not yet emitted (always less than one chunk between calls).
    private var pending: [Int16] = []
    /// Capture time of `pending[0]`, µs.
    private var pendingStartMicros = 0.0

    public init(sampleRate: Int = 48_000, channels: Int = 2, framesPerChunk: Int = 480,
                gapToleranceMicros: Double = 5_000) {
        self.sampleRate = sampleRate
        self.channels = channels
        self.framesPerChunk = framesPerChunk
        self.gapToleranceMicros = gapToleranceMicros
        pending.reserveCapacity(framesPerChunk * channels * 2)
    }

    private var microsPerFrame: Double { 1e6 / Double(sampleRate) }
    public var pendingFrames: Int { pending.count / channels }

    /// Drops the pending partial chunk (e.g. when audio is switched off).
    public mutating func reset() {
        pending.removeAll(keepingCapacity: true)
    }

    /// Appends `frameCount` frames, the first captured at `captureMicros`; `sample(frame,
    /// channel)` returns each sample as a float (nominal −1...1). Returns the chunks completed.
    public mutating func append(frameCount: Int, captureMicros: UInt64,
                                sample: (_ frame: Int, _ channel: Int) -> Float) -> [AudioMessage] {
        guard frameCount > 0 else { return [] }
        let start = Double(captureMicros)
        if pending.isEmpty {
            pendingStartMicros = start
        } else {
            let expected = pendingStartMicros + Double(pendingFrames) * microsPerFrame
            if abs(start - expected) > gapToleranceMicros {
                discardedFrames += pendingFrames
                pending.removeAll(keepingCapacity: true)
                pendingStartMicros = start
            } else {
                pendingStartMicros = start - Double(pendingFrames) * microsPerFrame
            }
        }
        for frame in 0..<frameCount {
            for channel in 0..<channels {
                pending.append(PCM16.sample(sample(frame, channel)))
            }
        }
        var chunks: [AudioMessage] = []
        let samplesPerChunk = framesPerChunk * channels
        var consumed = 0
        while pending.count - consumed >= samplesPerChunk {
            var pcm = Data(count: samplesPerChunk * 2)
            pcm.withUnsafeMutableBytes { raw in
                let out = raw.bindMemory(to: Int16.self)
                for i in 0..<samplesPerChunk { out[i] = pending[consumed + i].littleEndian }
            }
            let frameOffset = Double(consumed / channels)
            let micros = (pendingStartMicros + frameOffset * microsPerFrame).rounded()
            chunks.append(AudioMessage(captureTimeMicros: UInt64(max(0, micros)), pcm: pcm))
            consumed += samplesPerChunk
        }
        if consumed > 0 {
            pendingStartMicros += Double(consumed / channels) * microsPerFrame
            pending.removeFirst(consumed)
        }
        return chunks
    }
}
