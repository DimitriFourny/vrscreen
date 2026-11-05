import Foundation
import MirrorProtocol

/// AUDIO_CONFIG / AUDIO handling for the viewer: per-second stats (chunks/s, kbit/s, gaps),
/// an optional WAV file of everything received, and beep-onset detection for `--av-sync`.
/// All on the viewer queue.
final class AudioReceiver {
    /// A timestamp further than this from where the previous chunk ended counts as a gap
    /// (later) or an overlap (earlier).
    static let gapToleranceMicros = 1_000.0
    /// `--av-sync`: a sample above this (|s16|) after at least `onsetQuietFrames` of quiet
    /// marks a beep onset.
    static let onsetThreshold: Int16 = 1_000
    static let onsetQuietMicros = 100_000.0

    private(set) var config: AudioConfigMessage?
    /// Called with the capture time (µs, sender clock) of each detected beep onset.
    var onBeepOnset: ((Double) -> Void)?
    private let detectOnsets: Bool
    private var wav: WAVWriter?
    /// Where to write the WAV; cleared if the file cannot be created.
    private var wavPath: String?
    private var nextExpectedMicros: Double?
    private var quietFrames = Int.max / 2

    private var stats = Stats()
    struct Stats {
        var chunks = 0
        var pcmBytes = 0
        var gaps = 0
        var gapMicros = 0.0
        var overlaps = 0
        var unsupported = 0
        /// Arrival − captureTimeMicros per chunk (sender clock via PING/PONG offset), ms.
        var latenciesMs: [Double] = []
    }

    init(wavPath: String?, detectOnsets: Bool) {
        self.wavPath = wavPath
        self.detectOnsets = detectOnsets
    }

    func handleConfig(_ newConfig: AudioConfigMessage) {
        if newConfig != config {
            log("audio: AUDIO_CONFIG format \(newConfig.format), \(newConfig.sampleRate) Hz, \(newConfig.channels) ch")
        }
        config = newConfig
        nextExpectedMicros = nil
    }

    /// A new connection: the next chunk starts a new timeline (no gap counted).
    func connectionReset() {
        config = nil
        nextExpectedMicros = nil
    }

    /// `arrivalMicros`: when the chunk arrived, on the sender clock (nil before clock sync).
    func handle(_ chunk: AudioMessage, arrivalMicros: Int64?) {
        guard let config, config.format == AudioConfigMessage.pcmS16LE, config.channels > 0, config.sampleRate > 0,
              chunk.pcm.count % config.bytesPerFrame == 0 else {
            stats.unsupported += 1
            return
        }
        let rate = Double(config.sampleRate)
        let channels = Int(config.channels)
        let frames = chunk.pcm.count / config.bytesPerFrame
        let start = Double(chunk.captureTimeMicros)
        var gapFrames = 0
        if let expected = nextExpectedMicros {
            let delta = start - expected
            if delta > Self.gapToleranceMicros {
                stats.gaps += 1
                stats.gapMicros += delta
                gapFrames = Int((delta * rate / 1e6).rounded())
            } else if delta < -Self.gapToleranceMicros {
                stats.overlaps += 1
            }
        }
        nextExpectedMicros = start + Double(frames) * 1e6 / rate
        stats.chunks += 1
        stats.pcmBytes += chunk.pcm.count
        if let arrivalMicros {
            stats.latenciesMs.append(Double(arrivalMicros - Int64(chunk.captureTimeMicros)) / 1000)
        }

        if let wavPath {
            if wav == nil {
                do {
                    wav = try WAVWriter(path: wavPath, sampleRate: config.sampleRate, channels: config.channels,
                                        firstSampleMicros: chunk.captureTimeMicros)
                    log("audio-wav: writing \(wavPath): \(config.sampleRate) Hz, \(config.channels) ch, s16le; "
                        + "first sample captured at \(chunk.captureTimeMicros) µs (sender clock); gaps filled with silence")
                    gapFrames = 0
                } catch {
                    log("audio-wav: \(error)")
                    self.wavPath = nil
                }
            }
            if let wav {
                if wav.sampleRate == config.sampleRate && wav.channels == config.channels {
                    wav.append(silenceFrames: min(gapFrames, Int(rate) * 10), pcm: chunk.pcm)
                }
            }
        }

        if detectOnsets {
            chunk.pcm.withUnsafeBytes { raw in
                let quietNeeded = Int(Self.onsetQuietMicros * rate / 1e6)
                if gapFrames > 0 { quietFrames = min(quietFrames + gapFrames, Int.max / 2) }
                for frame in 0..<frames {
                    var loud = false
                    for channel in 0..<channels {
                        let s = Int16(littleEndian: raw.loadUnaligned(fromByteOffset: 2 * (frame * channels + channel),
                                                                       as: Int16.self))
                        if s > Self.onsetThreshold || s < -Self.onsetThreshold { loud = true }
                    }
                    if loud {
                        if quietFrames >= quietNeeded { onBeepOnset?(start + Double(frame) * 1e6 / rate) }
                        quietFrames = 0
                    } else {
                        quietFrames = min(quietFrames + 1, Int.max / 2)
                    }
                }
            }
        }
    }

    /// "audio 100.0 chunks/s 1536 kbit/s, gaps 0, arrival−capture p50 … p95 … max … ms"
    /// for the last `dt` seconds; nil before any audio.
    func takeStatsText(dt: Double) -> String? {
        defer { stats = Stats() }
        guard config != nil || stats.chunks > 0 else { return nil }
        var text = String(format: "audio %.1f chunks/s %.0f kbit/s, gaps %d", Double(stats.chunks) / dt,
                          Double(stats.pcmBytes) * 8 / dt / 1000, stats.gaps)
        if stats.gaps > 0 { text += String(format: " (%.1f ms)", stats.gapMicros / 1000) }
        if stats.overlaps > 0 { text += ", overlaps \(stats.overlaps)" }
        if stats.unsupported > 0 { text += ", \(stats.unsupported) unsupported" }
        if !stats.latenciesMs.isEmpty {
            let sorted = stats.latenciesMs.sorted()
            func percentile(_ p: Double) -> Double { sorted[min(sorted.count - 1, Int(Double(sorted.count) * p))] }
            text += String(format: ", arrival−capture p50 %.1f p95 %.1f max %.1f ms",
                           percentile(0.5), percentile(0.95), sorted.last!)
        }
        wav?.updateHeader()
        return text
    }

    /// Writes the final WAV sizes (call before exiting).
    func finish() {
        guard let wav else { return }
        wav.updateHeader()
        log(String(format: "audio-wav: %@ finished, %.2f s", wav.path, wav.durationSeconds))
    }
}

/// Minimal 16-bit PCM WAV writer; the header sizes are rewritten by `updateHeader`.
final class WAVWriter {
    let sampleRate: UInt32
    let channels: UInt8
    let firstSampleMicros: UInt64
    let path: String
    private let handle: FileHandle
    private var dataBytes = 0

    init(path: String, sampleRate: UInt32, channels: UInt8, firstSampleMicros: UInt64) throws {
        self.path = path
        guard FileManager.default.createFile(atPath: path, contents: nil),
              let handle = FileHandle(forWritingAtPath: path) else {
            throw OptionError("cannot create \(path)")
        }
        self.handle = handle
        self.sampleRate = sampleRate
        self.channels = channels
        self.firstSampleMicros = firstSampleMicros
        handle.write(header())
    }

    var durationSeconds: Double { Double(dataBytes) / Double(Int(channels) * 2) / Double(sampleRate) }

    func append(silenceFrames: Int, pcm: Data) {
        if silenceFrames > 0 {
            let silence = Data(count: silenceFrames * Int(channels) * 2)
            handle.write(silence)
            dataBytes += silence.count
        }
        handle.write(pcm) // already s16 little-endian, as WAV wants
        dataBytes += pcm.count
    }

    func updateHeader() {
        let position = handle.offsetInFile
        handle.seek(toFileOffset: 0)
        handle.write(header())
        handle.seek(toFileOffset: position)
    }

    private func header() -> Data {
        var d = Data()
        func le<T: FixedWidthInteger>(_ v: T) { Swift.withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
        let blockAlign = UInt16(channels) * 2
        d.append(contentsOf: Array("RIFF".utf8)); le(UInt32(36 + dataBytes))
        d.append(contentsOf: Array("WAVE".utf8))
        d.append(contentsOf: Array("fmt ".utf8)); le(UInt32(16)); le(UInt16(1)); le(UInt16(channels))
        le(sampleRate); le(sampleRate * UInt32(blockAlign)); le(blockAlign); le(UInt16(16))
        d.append(contentsOf: Array("data".utf8)); le(UInt32(dataBytes))
        return d
    }
}
