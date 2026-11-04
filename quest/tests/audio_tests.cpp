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

// Sample n: left = n % 30000 + 1, right = n / 30000 + 1 (0 is silence).
int16_t leftOf(int64_t n) { return static_cast<int16_t>(n % 30000 + 1); }
int16_t rightOf(int64_t n) { return static_cast<int16_t>(n / 30000 + 1); }
// Index heard, or -1 for silence / a fade / the ramp's wrap (ambiguous under interpolation).
double decode(int16_t left, int16_t right) {
    if (left <= 0 || right <= 0 || left < 3 || left > 29997) return -1.0;
    return static_cast<double>(right - 1) * 30000.0 + left - 1;
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

    Sim() { playout.reset(kRate, 2, kRate); }
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
            const int64_t present = nextCallback() + kDeviceLatency;
            playout.render(buffer.data(), kBurst, present);
            ++callbacks;
            heard.push_back({present, decode(buffer[0], buffer[1])});
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
    // Worst arrival: 100 ms stall + 10 ms chunk + 5 ms transit, + 10 ms margin.
    CHECK_NEAR(s.targetDelayMicros, 125'000, 1'000);
    // Re-synced at the new target (no 200 ms jump needed: it waited for the burst).
    const Sim::Delay d = sim.delay(2.5, 3.0);
    CHECK_NEAR(d.min, 125.0, 6.0);
    CHECK_NEAR(d.max, 125.0, 6.0);
}

TEST(audioTargetFallsBackAfterTheJitterWindow) {
    Sim sim;
    sim.transit = [](int64_t k) { return k == 50 ? int64_t{60'000} : int64_t{5'000}; };
    sim.run(1.0);
    CHECK(sim.playout.targetDelayMicros() > 70'000);
    sim.run(5.0);  // the spike left the 4 s window
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

}  // namespace
