import Foundation
import Testing
@testable import MirrorProtocol

@Test func audioMessageLayouts() throws {
    // AUDIO_CONFIG s16le, 48000 Hz, 2 ch (the example in docs/protocol.md).
    let config = Message.audioConfig(.standard).encoded()
    #expect(config == Data([0x04, 0, 0, 0, 6, 0x01, 0x00, 0x00, 0xBB, 0x80, 0x02]))
    // AUDIO: big-endian header, little-endian samples. One stereo frame L = 1, R = -2.
    let audio = Message.audio(AudioMessage(captureTimeMicros: 0x0102_0304_0506_0708,
                                           pcm: Data([0x01, 0x00, 0xFE, 0xFF]))).encoded()
    #expect(audio == Data([0x05, 0, 0, 0, 12, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x01, 0x00, 0xFE, 0xFF]))
    #expect(AudioConfigMessage.standard.bytesPerFrame == 4)
}

@Test func audioDecodeRules() throws {
    #expect(try Message.decode(type: 0x04, payload: Data([1, 0, 0, 0xAC, 0x44, 1, 0xEE]))
        == .audioConfig(AudioConfigMessage(format: 1, sampleRate: 44_100, channels: 1))) // extra byte ignored
    // Unknown formats decode; the receiver decides to ignore them.
    #expect(try Message.decode(type: 0x04, payload: Data([9, 0, 0, 0xBB, 0x80, 2]))
        == .audioConfig(AudioConfigMessage(format: 9, sampleRate: 48_000, channels: 2)))
    #expect(throws: ProtocolError.self) { try Message.decode(type: 0x04, payload: Data([1, 0, 0, 0xBB, 0x80])) }
    #expect(try Message.decode(type: 0x05, payload: Data(repeating: 0, count: 8))
        == .audio(AudioMessage(captureTimeMicros: 0, pcm: Data())))
    #expect(throws: ProtocolError.self) { try Message.decode(type: 0x05, payload: Data(repeating: 0, count: 7)) }
}

@Test func floatToS16Conversion() {
    #expect(PCM16.sample(0) == 0)
    #expect(PCM16.sample(1) == 32767)
    #expect(PCM16.sample(-1) == -32767)
    #expect(PCM16.sample(0.5) == 16384) // 16383.5 rounds away from zero
    #expect(PCM16.sample(-0.5) == -16384)
    #expect(PCM16.sample(1.5) == .max)
    #expect(PCM16.sample(-2) == .min)
    #expect(PCM16.sample(.infinity) == .max)
    #expect(PCM16.sample(-.infinity) == .min)
    #expect(PCM16.sample(.nan) == 0)
    #expect(PCM16.sample(1.0 / 65534) == 1) // half an LSB rounds up
}

/// Little-endian s16 samples of a chunk.
private func samples(_ chunk: AudioMessage) -> [Int16] {
    chunk.pcm.withUnsafeBytes { raw in
        (0..<chunk.pcm.count / 2).map { Int16(littleEndian: raw.loadUnaligned(fromByteOffset: $0 * 2, as: Int16.self)) }
    }
}

@Test func chunkerRechunksAndInterleaves() {
    var chunker = AudioChunker()
    // SCK-style 960-frame buffers, planar float: left = frame index / 32767, right = negative.
    var chunks: [AudioMessage] = []
    var frameBase = 0
    for buffer in 0..<3 {
        let base = frameBase
        chunks += chunker.append(frameCount: 960, captureMicros: 1_000_000 + UInt64(buffer) * 20_000) { frame, channel in
            Float((base + frame) % 1000) / 32767 * (channel == 0 ? 1 : -1)
        }
        frameBase += 960
    }
    #expect(chunks.count == 6)
    #expect(chunks.map(\.captureTimeMicros) == (0..<6).map { 1_000_000 + UInt64($0) * 10_000 })
    #expect(chunks.allSatisfy { $0.pcm.count == 480 * 2 * 2 })
    // Interleaved L R L R ..., little-endian on the wire.
    #expect(Array(samples(chunks[1]).prefix(4)) == [480, -480, 481, -481])
    #expect(Array(chunks[1].pcm.prefix(4)) == [0xE0, 0x01, 0x20, 0xFE])
    #expect(chunker.pendingFrames == 0)
}

@Test func chunkerKeepsTimestampsContinuousAcrossOddBuffers() {
    var chunker = AudioChunker()
    var chunks: [AudioMessage] = []
    var micros = 5_000_000.0
    // 1000-frame buffers (20.833 ms): chunks straddle buffer boundaries.
    for _ in 0..<12 {
        chunks += chunker.append(frameCount: 1000, captureMicros: UInt64(micros.rounded())) { _, _ in 0.25 }
        micros += 1000 * 1e6 / 48_000
    }
    #expect(chunks.count == 25) // 12000 frames = 25 chunks
    #expect(chunker.pendingFrames == 0)
    for (i, chunk) in chunks.enumerated() {
        let expected = 5_000_000 + Int64(i) * 10_000
        #expect(abs(Int64(chunk.captureTimeMicros) - expected) <= 1, "chunk \(i)")
    }
    #expect(samples(chunks[3]).allSatisfy { $0 == 8192 })
    #expect(chunker.discardedFrames == 0)
}

@Test func chunkerFollowsSmallJitterAndRestartsAfterGaps() {
    var chunker = AudioChunker()
    // 720 frames: one chunk out, 240 frames pending.
    var chunks = chunker.append(frameCount: 720, captureMicros: 1_000_000) { _, _ in 0 }
    #expect(chunks.map(\.captureTimeMicros) == [1_000_000])
    // Next buffer 300 µs late (jitter, within tolerance): the pending frames are re-timed so the
    // new buffer starts exactly at its own timestamp.
    chunks = chunker.append(frameCount: 240, captureMicros: 1_015_300) { _, _ in 0 }
    #expect(chunks.map(\.captureTimeMicros) == [1_010_300])
    #expect(chunker.discardedFrames == 0)
    // 240 pending again, then a 50 ms gap: the partial chunk is discarded, timing restarts.
    _ = chunker.append(frameCount: 240, captureMicros: 1_020_300) { _, _ in 0 }
    chunks = chunker.append(frameCount: 480, captureMicros: 1_075_300) { _, _ in 0 }
    #expect(chunker.discardedFrames == 240)
    #expect(chunks.map(\.captureTimeMicros) == [1_075_300])
    // Time going backwards is a discontinuity too.
    _ = chunker.append(frameCount: 100, captureMicros: 2_000_000) { _, _ in 0 }
    chunks = chunker.append(frameCount: 480, captureMicros: 1_900_000) { _, _ in 0 }
    #expect(chunker.discardedFrames == 340)
    #expect(chunks.map(\.captureTimeMicros) == [1_900_000])
    // reset() drops pending frames without counting them.
    _ = chunker.append(frameCount: 100, captureMicros: 3_000_000) { _, _ in 0 }
    chunker.reset()
    #expect(chunker.pendingFrames == 0)
    chunks = chunker.append(frameCount: 480, captureMicros: 3_000_000) { _, _ in 0 }
    #expect(chunks.map(\.captureTimeMicros) == [3_000_000])
}

@Test func chunkerOtherRatesAndMono() {
    var chunker = AudioChunker(sampleRate: 44_100, channels: 1, framesPerChunk: 441)
    let chunks = chunker.append(frameCount: 882, captureMicros: 0) { frame, _ in frame == 441 ? -1 : 0 }
    #expect(chunks.map(\.captureTimeMicros) == [0, 10_000])
    #expect(chunks.map(\.pcm.count) == [882, 882])
    #expect(samples(chunks[1])[0] == -32767)
}
