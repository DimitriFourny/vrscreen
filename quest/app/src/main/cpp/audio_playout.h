#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace qm {

// Counters since the previous takeStats() (latency figures cover callbacks that played audio).
struct AudioPlayoutStats {
    int64_t bufferedMicros = -1;      // received but not yet played, at the last callback; -1 = idle
    int64_t targetDelayMicros = 0;    // capture -> speaker delay aimed at
    int64_t latencySumMicros = 0;     // capture -> speaker, per callback
    int64_t latencyMaxMicros = 0;
    int64_t latencySamples = 0;
    uint64_t underruns = 0;           // ran out of received audio while playing (silence, then re-sync)
    uint64_t resyncs = 0;             // jumps: error beyond jumpMicros, or the timeline restarted
    uint64_t lateChunks = 0;          // arrived after their samples' playout time
    uint64_t droppedChunks = 0;       // duplicates / older than what was already received
    uint64_t gaps = 0;                // holes in the capture timeline, filled with silence
    int64_t gapMicros = 0;
    uint64_t corrections = 0;         // drift corrections started (error beyond correctStartMicros)
    int64_t correctedMicros = 0;      // source time skipped (+) or stretched (-) by them
};

// Jitter buffer and playout scheduler for the Mac's PCM audio, between the network thread
// (producer: push) and the audio device callback (consumer: render). Single producer, single
// consumer, no locks: a ring of interleaved s16 frames on a capture timeline plus atomics.
//
// Timeline: received frames get consecutive indices; an anchor (index, sender capture time) maps
// indices to sender time and, with the PING clock offset, to local time. Chunks that continue the
// previous one are appended (their timestamps only nudge the anchor, filtering timestamp jitter);
// a hole in the capture times is filled with silence; duplicates are dropped.
//
// Playout: frame i is meant to be heard at localCapture(i) + targetDelay. The target is the larger
// of minDelay (40 ms), the video's capture->display delay (setVideoDelay) and the worst arrival
// delay of the last 4 s plus a margin; at most maxDelay (150 ms). render() gets the time its
// first frame will be heard (from AAudioStream_getTimestamp) and:
// - waiting (start, after an underrun or a jump): silence until the frame due now has arrived,
//   then starts exactly there with a short fade-in;
// - playing: compares when the current frame will be heard with when it is due. Beyond 5 ms
//   (smoothed) it resamples by a tiny ratio (0.25 x the error per second, at most 2 %) until the
//   error is under 1 ms: inaudible drift correction, no clicks. Beyond 200 ms it fades out and
//   re-syncs (jump). Running out of data fades out, plays silence and re-syncs.
// Linear interpolation also covers a device that refused the source rate (outputRate differs).
class AudioPlayout {
public:
    static constexpr int kMaxChannels = 2;
    static constexpr int64_t kCapacityFrames = 1 << 16;  // 1.37 s at 48 kHz

    struct Config {
        int64_t minDelayMicros = 40'000;
        int64_t maxDelayMicros = 150'000;
        int64_t marginMicros = 10'000;      // over the worst arrival delay: device callback lead
        int64_t jumpMicros = 200'000;
        int64_t correctStartMicros = 5'000;
        int64_t correctStopMicros = 1'000;
        double gainPerSecond = 0.25;        // resample ratio per second of error
        double maxRatio = 0.02;
        double errorSmoothingSeconds = 0.3;
        int64_t gapToleranceMicros = 5'000;  // timestamp jitter still counted as contiguous
        int fadeFrames = 96;                 // 2 ms at 48 kHz
        int64_t jitterBucketMicros = 250'000;  // arrival-delay window: 16 buckets = 4 s
    };

    AudioPlayout() : AudioPlayout(Config{}) {}
    explicit AudioPlayout(const Config& config);

    // ---- Producer (network thread) ----
    // New format or new connection: forgets everything. The consumer must not be running.
    // `outputRate`: the device's rate (normally == sampleRate).
    void reset(int sampleRate, int channels, int outputRate);
    // Sender clock minus local clock (StreamClient::clockOffset), whenever known/updated.
    void setClockOffset(int64_t senderMinusLocalMicros);
    // The video's capture -> display delay: audio does not play earlier than the picture.
    void setVideoDelay(int64_t micros);
    // `frames` interleaved s16 frames (native endianness, unaligned is fine) whose first sample
    // was captured at `captureMicros` (sender clock), received at `arrivalMicros` (local clock).
    void push(int64_t captureMicros, const void* samples, int frames, int64_t arrivalMicros);

    // ---- Consumer (audio callback; wait-free) ----
    // Fills `frames` interleaved frames (channels() each) whose first will be heard at
    // `presentMicros` (local clock).
    void render(int16_t* out, int frames, int64_t presentMicros);

    // Any thread. Returns and clears the counters.
    AudioPlayoutStats takeStats();
    int sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }
    int64_t targetDelayMicros() const { return targetDelay_.load(std::memory_order_relaxed); }

private:
    struct Anchor {
        int64_t index;
        int64_t micros;  // sender capture time of `index`
    };
    void publishAnchor(int64_t index, int64_t micros);
    bool readAnchor(Anchor* out) const;
    void writeFrames(int64_t at, const void* samples, int frames);
    void writeSilence(int64_t at, int64_t frames);
    void updateTarget(int64_t captureMicros, int64_t arrivalMicros);

    const Config config_;
    int sampleRate_ = 48000;
    int channels_ = 2;
    int outputRate_ = 48000;
    std::vector<int16_t> ring_;

    // Shared.
    std::atomic<int64_t> writeIndex_{0};   // frames [oldest, writeIndex) are valid
    std::atomic<int64_t> oldestIndex_{0};
    std::atomic<uint32_t> anchorSeq_{0};   // seqlock around the anchor
    std::atomic<int64_t> anchorIndex_{0};
    std::atomic<int64_t> anchorMicros_{0};
    std::atomic<bool> haveAnchor_{false};
    std::atomic<bool> offsetValid_{false};
    std::atomic<int64_t> offset_{0};
    std::atomic<int64_t> targetDelay_{0};
    std::atomic<int64_t> playhead_{-1};    // next index the consumer plays (late detection)

    // Producer only.
    int64_t videoDelay_ = 0;
    int64_t bucketStart_ = 0;
    int bucket_ = 0;
    int64_t bucketMax_[16] = {};

    // Consumer only.
    bool playing_ = false;
    int64_t readIndex_ = 0;
    double frac_ = 0.0;
    double errorSmooth_ = 0.0;  // microseconds
    bool correcting_ = false;
    int fadeIn_ = 0;

    // Stats.
    std::atomic<int64_t> statBuffered_{-1};
    std::atomic<int64_t> statLatencySum_{0};
    std::atomic<int64_t> statLatencyMax_{0};
    std::atomic<int64_t> statLatencySamples_{0};
    std::atomic<uint64_t> statUnderruns_{0};
    std::atomic<uint64_t> statResyncs_{0};
    std::atomic<uint64_t> statLate_{0};
    std::atomic<uint64_t> statDropped_{0};
    std::atomic<uint64_t> statGaps_{0};
    std::atomic<int64_t> statGapMicros_{0};
    std::atomic<uint64_t> statCorrections_{0};
    std::atomic<int64_t> statCorrectedNanos_{0};
};

}  // namespace qm
