// Host tests for AudioPlayout: a simulated sender (chunks of 480 frames with capture timestamps,
// a transit delay per chunk, optional clock drift and dropped chunks) and a simulated AAudio
// device (callbacks of 192 frames heard 8 ms later). The source signal encodes each sample's
// index, so the output says exactly which captured sample is heard when.

#include "audio_playout.h"
#include "test_support.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

using namespace qm;

namespace {

constexpr int kRate = 48000;
constexpr int kChunk = 480;  // 10 ms
constexpr int kBurst = 192;  // device callback size (4 ms)
constexpr int64_t kDeviceLatency = 8'000;

// Sample n: two ramps of period 30000 half a period apart, left = n % 30000 + 1 and
// right = (n + 15000) % 30000 + 1 (0 is silence). At most one of them is near its wrap (where
// interpolation between its last and first value, while correcting drift, means nothing).
int16_t leftOf(int64_t n) { return static_cast<int16_t>(n % 30000 + 1); }
int16_t rightOf(int64_t n) { return static_cast<int16_t>((n + 15000) % 30000 + 1); }
// Index heard, given a rough guess (within +-7000 samples), or -1 for silence or a fade (the
// ramps disagree).
double decode(int16_t left, int16_t right, double guess) {
    const bool leftOk = left >= 100 && left <= 29900;
    const bool rightOk = right >= 100 && right <= 29900;
    if (!leftOk && !rightOk) return -1.0;
    const double fromLeft = left - 1;
    const double fromRight = std::fmod(right - 1 + 15000.0, 30000.0);
    if (leftOk && rightOk && std::fabs(fromLeft - fromRight) > 2.0) return -1.0;
    const double low = leftOk ? fromLeft : fromRight;  // index modulo 30000
    return low + 30000.0 * std::round((guess - low) / 30000.0);
}

// Most tests switch the startup floor off to check the steady-state logic from the first second;
// the source-behaviour tests at the end use the real defaults.
AudioPlayout::Config withoutStartupFloor() {
    AudioPlayout::Config c;
    c.startupDelayMicros = 0;
    return c;
}

struct Sim {
    AudioPlayout playout;
    int outRate = kRate;
    int64_t offset = 7'000'000;      // sender clock = local clock + offset
    double ppm = 0.0;                 // sender sample clock vs sender host clock
    int64_t localStart = 1'000'000;
    std::function<int64_t(int64_t)> transit = [](int64_t) { return int64_t{5'000}; };  // after the chunk's end
    std::function<bool(int64_t)> sent = [](int64_t) { return true; };
    bool offsetKnown = true;
    int64_t deviceLatency = kDeviceLatency;  // callback -> heard

    int64_t chunk = 0;      // next chunk to send
    int64_t callbacks = 0;  // device callbacks so far
    int64_t lastArrival = 0;
    std::vector<int16_t> buffer = std::vector<int16_t>(kBurst * 2);

    // What was heard: (present time of a callback's first frame, decoded index or -1).
    struct Heard {
        int64_t present;
        double index;
    };
    std::vector<Heard> heard;
    int64_t silentFrames = 0;

    explicit Sim(const AudioPlayout::Config& config = withoutStartupFloor()) : playout(config) {
        playout.reset(kRate, 2, kRate);
    }
    void setOutputRate(int rate) {
        outRate = rate;
        playout.reset(kRate, 2, rate);
    }
    // Local time at which sample n was captured.
    double captureLocal(double n) const { return localStart + n * 1e6 / (kRate * (1.0 + ppm * 1e-6)); }

    void sendChunk() {
        const int64_t first = chunk * kChunk;
        int64_t arrival = static_cast<int64_t>(captureLocal(static_cast<double>(first + kChunk))) + transit(chunk);
        arrival = std::max(arrival, lastArrival);  // TCP: in order
        lastArrival = arrival;
        if (sent(chunk)) {
            std::vector<int16_t> pcm(kChunk * 2);
            for (int i = 0; i < kChunk; ++i) {
                pcm[2 * i] = leftOf(first + i);
                pcm[2 * i + 1] = rightOf(first + i);
            }
            if (offsetKnown) playout.setClockOffset(offset);
            playout.push(static_cast<int64_t>(captureLocal(static_cast<double>(first))) + offset, pcm.data(), kChunk,
                         arrival);
        }
        ++chunk;
    }
    int64_t nextArrival() const {
        const int64_t first = chunk * kChunk;
        return std::max(static_cast<int64_t>(captureLocal(static_cast<double>(first + kChunk))) + transit(chunk),
                        lastArrival);
    }
    int64_t nextCallback() const { return localStart + callbacks * kBurst * 1'000'000 / outRate; }

    void run(double seconds) {
        const int64_t end = nextCallback() + static_cast<int64_t>(seconds * 1e6);
        while (nextCallback() < end) {
            if (nextArrival() <= nextCallback()) {
                sendChunk();
                continue;
            }
            const int64_t present = nextCallback() + deviceLatency;
            playout.render(buffer.data(), kBurst, present, nextCallback());
            ++callbacks;
            const double guess = (present - 95'000 - localStart) * kRate * (1.0 + ppm * 1e-6) / 1e6;
            heard.push_back({present, decode(buffer[0], buffer[1], guess)});
            for (int i = 0; i < kBurst; ++i) silentFrames += buffer[2 * i] == 0 ? 1 : 0;
        }
    }
    // Capture -> speaker delay of the heard callbacks in [from, to) seconds of the run.
    struct Delay {
        double min = 1e18, max = -1e18;
        int samples = 0;
    };
    Delay delay(double fromSeconds, double toSeconds) const {
        Delay d;
        for (const Heard& h : heard) {
            const double t = static_cast<double>(h.present - localStart) / 1e6;
            if (t < fromSeconds || t >= toSeconds || h.index < 0) continue;
            const double ms = (static_cast<double>(h.present) - captureLocal(h.index)) / 1000.0;
            d.min = std::min(d.min, ms);
            d.max = std::max(d.max, ms);
            ++d.samples;
        }
        return d;
    }
};

TEST(audioSilentUntilTheClockOffsetIsKnown) {
    Sim sim;
    sim.offsetKnown = false;
    sim.run(0.5);
    CHECK(sim.silentFrames == sim.callbacks * kBurst);
    CHECK(sim.playout.takeStats().bufferedMicros == -1);
}

TEST(audioSteadyStatePlaysEverySampleAtTheTargetDelay) {
    Sim sim;
    sim.run(3.0);
    const AudioPlayoutStats s = sim.playout.takeStats();
    CHECK(s.underruns == 0);
    CHECK(s.resyncs == 0);
    CHECK(s.corrections == 0);
    CHECK_NEAR(s.targetDelayMicros, 40'000, 0);  // transit 15 ms + margin < the 40 ms floor
    const Sim::Delay d = sim.delay(0.2, 3.0);
    CHECK(d.samples > 600);
    CHECK_NEAR(d.min, 40.0, 0.05);
    CHECK_NEAR(d.max, 40.0, 0.05);
    // Exact copies (no correction -> no interpolation): consecutive callbacks heard consecutive samples.
    int consecutive = 0;
    for (size_t i = 1; i < sim.heard.size(); ++i) {
        if (sim.heard[i].index >= 0 && sim.heard[i - 1].index >= 0) {
            consecutive += sim.heard[i].index - sim.heard[i - 1].index == kBurst ? 1 : 0;
        }
    }
    CHECK(consecutive > 600);
    CHECK_NEAR(static_cast<double>(s.latencySumMicros) / static_cast<double>(s.latencySamples), 40'000, 50);
}

TEST(audioFollowsTheVideoDelayWhenLarger) {
    Sim sim;
    sim.playout.setVideoDelay(55'000);
    sim.run(4.0);
    const Sim::Delay d = sim.delay(3.0, 4.0);
    CHECK_NEAR(d.min, 55.0, 1.5);
    CHECK_NEAR(d.max, 55.0, 1.5);
}

TEST(audioShortGapIsFilledWithSilenceWithoutResync) {
    Sim sim;
    sim.sent = [](int64_t k) { return k != 100; };  // 10 ms dropped by the sender
    sim.run(0.5);
    const int64_t silentAtStart = sim.silentFrames;
    sim.run(2.5);
    const AudioPlayoutStats s = sim.playout.takeStats();
    CHECK(s.gaps == 1);
    CHECK_NEAR(s.gapMicros, 10'000, 100);
    CHECK(s.underruns == 0 && s.resyncs == 0);
    CHECK(sim.silentFrames - silentAtStart == kChunk);  // exactly the missing 10 ms, in place
    const Sim::Delay d = sim.delay(1.2, 3.0);  // after the gap: same timing
    CHECK_NEAR(d.min, 40.0, 0.05);
    CHECK_NEAR(d.max, 40.0, 0.05);
}

TEST(audioGapLongerThanTheBufferUnderrunsButKeepsTiming) {
    Sim sim;
    sim.sent = [](int64_t k) { return k != 100 && k != 101 && k != 102; };  // 30 ms: refilled too late
    sim.run(3.0);
    const AudioPlayoutStats s = sim.playout.takeStats();
    CHECK(s.underruns == 1);
    const Sim::Delay d = sim.delay(1.2, 3.0);
    CHECK_NEAR(d.min, 40.0, 0.05);
    CHECK_NEAR(d.max, 40.0, 0.05);
}

TEST(audioLongGapUnderrunsThenResyncs) {
    Sim sim;
    sim.sent = [](int64_t k) { return k < 100 || k >= 130; };  // 300 ms of nothing
    sim.run(3.0);
    const AudioPlayoutStats s = sim.playout.takeStats();
    CHECK(s.underruns == 1);
    CHECK(s.gaps == 1);
    const Sim::Delay d = sim.delay(1.6, 3.0);
    CHECK_NEAR(d.min, 40.0, 0.2);
    CHECK_NEAR(d.max, 40.0, 0.2);
}

TEST(audioStallCountsLateChunksAndRaisesTheTarget) {
    Sim sim;
    // A 100 ms stall: chunks 100..109 arrive together with chunk 110 (TCP head-of-line).
    sim.transit = [](int64_t k) { return k >= 100 && k <= 110 ? int64_t{5'000} + (110 - k) * 10'000 : int64_t{5'000}; };
    sim.run(3.0);
    const AudioPlayoutStats s = sim.playout.takeStats();
    CHECK(s.underruns >= 1);
    CHECK(s.lateChunks >= 1);
    // Worst arrival: 100 ms stall + 10 ms chunk + 5 ms transit, + device lead (8 ms + one
    // 194-frame callback) + 5 ms. One underrun only: no boost.
    CHECK_NEAR(s.targetDelayMicros, 132'000, 1'000);
    // Re-synced at the new target (no 200 ms jump needed: it waited for the burst).
    const Sim::Delay d = sim.delay(2.5, 3.0);
    CHECK_NEAR(d.min, 132.0, 6.0);
    CHECK_NEAR(d.max, 132.0, 6.0);
}

TEST(audioTargetFallsBackAfterTheJitterWindow) {
    Sim sim;
    sim.transit = [](int64_t k) { return k == 50 ? int64_t{60'000} : int64_t{5'000}; };
    sim.run(1.0);
    CHECK(sim.playout.targetDelayMicros() > 70'000);
    sim.run(9.0);  // the spike left the 8 s window
    CHECK_NEAR(sim.playout.targetDelayMicros(), 40'000, 0);
    sim.run(30.0);  // and the playout drifted back to it gently
    const Sim::Delay d = sim.delay(30.0, 36.0);
    CHECK(d.max < 41.5 && d.min > 38.5);
}

TEST(audioTargetIsCappedAt150ms) {
    Sim sim;
    sim.transit = [](int64_t k) { return k % 50 == 0 ? int64_t{400'000} : int64_t{5'000}; };
    sim.run(3.0);
    CHECK_NEAR(sim.playout.targetDelayMicros(), 150'000, 0);
}

void checkDrift(double ppm) {
    Sim sim;
    sim.ppm = ppm;
    sim.run(60.0);
    const AudioPlayoutStats s = sim.playout.takeStats();
    // Without correction the error would reach 60 s x ppm (18 ms at 300 ppm).
    const Sim::Delay d = sim.delay(1.0, 60.0);
    CHECK(d.max < 40.0 + 6.5);
    CHECK(d.min > 40.0 - 6.5);
    CHECK(s.corrections >= 1);
    CHECK(s.underruns == 0 && s.resyncs == 0);
    // Corrected about the drift: + = skipped ahead (sender faster).
    CHECK_NEAR(static_cast<double>(s.correctedMicros) / 1000.0, 60.0 * ppm * 1e-3, 6.0);
}

TEST(audioDriftIsCorrectedGentlyFasterSender) { checkDrift(300.0); }
TEST(audioDriftIsCorrectedGentlySlowerSender) { checkDrift(-300.0); }

TEST(audioSenderClockStepResyncs) {
    Sim sim;
    sim.run(1.0);
    // The sender's clock steps 300 ms forward (timestamps jump, the samples do not) and the PING
    // offset follows: a 300 ms "hole" appears in the capture timeline, the mapping moves with it.
    sim.offset += 300'000;
    sim.run(2.0);
    const AudioPlayoutStats s = sim.playout.takeStats();
    CHECK(s.resyncs == 1);
    CHECK(s.gaps == 1);
    // Afterwards every sample is again heard 40 ms after its real capture.
    const Sim::Delay d = sim.delay(1.5, 3.0);
    CHECK(d.samples > 300);
    CHECK_NEAR(d.min, 40.0, 0.05);
    CHECK_NEAR(d.max, 40.0, 0.05);
}

TEST(audioDeviceAtAnotherRateIsResampled) {
    Sim sim;
    sim.setOutputRate(44100);
    sim.run(5.0);
    const AudioPlayoutStats s = sim.playout.takeStats();
    CHECK(s.underruns == 0 && s.resyncs == 0);
    const Sim::Delay d = sim.delay(1.0, 5.0);
    CHECK_NEAR(d.min, 40.0, 0.2);
    CHECK_NEAR(d.max, 40.0, 0.2);
}

TEST(audioDuplicateChunkIsDropped) {
    AudioPlayout p;
    p.reset(kRate, 2, kRate);
    p.setClockOffset(0);
    std::vector<int16_t> pcm(kChunk * 2, 100);
    p.push(1'000'000, pcm.data(), kChunk, 1'015'000);
    p.push(1'010'000, pcm.data(), kChunk, 1'025'000);
    p.push(1'000'000, pcm.data(), kChunk, 1'026'000);  // the first one again
    const AudioPlayoutStats s = p.takeStats();
    CHECK(s.droppedChunks == 1);
    CHECK(s.gaps == 0);
}

// Reproduces the headset log under 4K encode load: audio arrives in batches every 30-60 ms, each
// chunk 30-110 ms after its first sample was captured, and the device plays 20 ms after its
// callback. The playout must settle at a delay that covers all of it (not underrun continuously).
TEST(audioBurstyLateArrivalsSettleWithoutUnderruns) {
    Sim sim(AudioPlayout::Config{});
    sim.deviceLatency = 20'000;
    // Deterministic pseudo-random batches.
    uint32_t seed = 12345;
    auto next = [&seed](int lo, int hi) {
        seed = seed * 1664525u + 1013904223u;
        return lo + static_cast<int>((seed >> 8) % static_cast<uint32_t>(hi - lo + 1));
    };
    const int64_t chunks = 2500;  // 25 s
    std::vector<int64_t> batchTimes;
    for (int64_t t = sim.localStart; t < sim.localStart + 30'000'000; t += next(30, 60) * 1000) batchTimes.push_back(t);
    std::vector<int64_t> transits(static_cast<size_t>(chunks));
    int64_t worst = 0;
    for (int64_t k = 0; k < chunks; ++k) {
        const int64_t captureEnd = sim.localStart + (k + 1) * 10'000;
        const int64_t ready = captureEnd + next(20, 40) * 1000;
        const int64_t arrival = *std::lower_bound(batchTimes.begin(), batchTimes.end(), ready);
        transits[static_cast<size_t>(k)] = arrival - captureEnd;
        worst = std::max(worst, arrival - captureEnd + 10'000);  // from the chunk's first sample
    }
    CHECK(worst > 90'000 && worst <= 110'000);
    sim.transit = [&transits](int64_t k) { return transits[static_cast<size_t>(std::min<int64_t>(k, 2499))]; };
    sim.run(5.0);
    sim.playout.takeStats();
    sim.run(15.0);
    const AudioPlayoutStats s = sim.playout.takeStats();
    CHECK_NEAR(s.underruns, 0, 0);
    CHECK_NEAR(s.lateChunks, 0, 0);
    const Sim::Delay d = sim.delay(5.0, 20.0);
    CHECK(d.max <= 150.5);
    // Covering the worst arrivals plus the 24 ms device lead, under the cap.
    CHECK(s.targetDelayMicros >= worst + 20'000 && s.targetDelayMicros <= 150'000);
    CHECK(d.min >= 100.0);
}

// "First seconds fine, then crackling": once settled (after the startup floor), the sender's
// arrival delay keeps growing, +20 ms/s for 5 s, then holds at 105 ms. The playout must re-sync
// to larger delays a few times rather than underrun every few callbacks.
TEST(audioGrowingArrivalDelayResyncsInsteadOfCrackling) {
    Sim sim(AudioPlayout::Config{});
    sim.deviceLatency = 20'000;
    sim.transit = [](int64_t k) {
        const int64_t t = k * 10'000;  // capture time since start
        const int64_t ramp = std::clamp<int64_t>(t - 10'000'000, 0, 5'000'000);
        return int64_t{5'000} + ramp * 20 / 1000;
    };
    sim.run(10.0);
    const AudioPlayoutStats settled = sim.playout.takeStats();
    CHECK_NEAR(settled.underruns, 0, 0);
    sim.run(7.0);
    const AudioPlayoutStats ramp = sim.playout.takeStats();
    CHECK_NEAR(ramp.underruns, 0, 5);
    sim.run(13.0);
    const AudioPlayoutStats hold = sim.playout.takeStats();
    CHECK_NEAR(hold.underruns, 0, 0);
    CHECK_NEAR(hold.lateChunks, 0, 0);
    const Sim::Delay d = sim.delay(18.0, 30.0);
    CHECK(d.min >= 115.0);   // 105 ms arrival + 10 ms chunk
    CHECK(d.max <= 150.5);
}

// What MacAudio measured on the Mac under 4K load: ScreenCaptureKit hands over audio in 20 ms
// buffers (split into two 10 ms chunks, which arrive together), about 10 ms after the buffer's
// end during the first second, then 31-48 ms after it in a 4 s sawtooth (arrival - capture of a
// chunk's first sample: up to ~70 ms). With a 20 ms device lead the playout must not underrun,
// neither while the delay settles nor after.
TEST(audioScreenCaptureKitArrivalPatternPlaysCleanly) {
    Sim sim(AudioPlayout::Config{});
    sim.deviceLatency = 20'000;
    sim.transit = [](int64_t k) {
        const int64_t batchEnd = (k / 2 + 1) * 20'000;  // since start
        const int64_t lateAfterEnd =
            batchEnd < 1'000'000 ? 10'000 : 31'000 + 17'000 * (batchEnd % 4'000'000) / 4'000'000;
        const int64_t chunkEnd = (k + 1) * 10'000;
        return batchEnd + lateAfterEnd - chunkEnd;  // both chunks of a batch arrive together
    };
    sim.run(30.0);
    const AudioPlayoutStats s = sim.playout.takeStats();
    CHECK_NEAR(s.underruns, 0, 0);
    CHECK_NEAR(s.lateChunks, 0, 0);
    CHECK_NEAR(s.resyncs, 0, 0);
    // Worst arrival 68 ms + 24 ms lead + 5 ms: about 97 ms once the startup floor (90) is over.
    CHECK_NEAR(sim.playout.targetDelayMicros(), 97'000, 2'000);
    const Sim::Delay d = sim.delay(20.0, 30.0);
    CHECK(d.samples > 2000);
    CHECK(d.min > 90.0 && d.max < 104.0);
}

}  // namespace
