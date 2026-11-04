#include "audio_playout.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace qm {
namespace {

constexpr int64_t kMask = AudioPlayout::kCapacityFrames - 1;
constexpr int kBuckets = 16;

void atomicMax(std::atomic<int64_t>& a, int64_t v) {
    int64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

}  // namespace

AudioPlayout::AudioPlayout(const Config& config) : config_(config) { reset(48000, 2, 48000); }

void AudioPlayout::reset(int sampleRate, int channels, int outputRate) {
    sampleRate_ = std::max(sampleRate, 1);
    channels_ = std::clamp(channels, 1, kMaxChannels);
    outputRate_ = std::max(outputRate, 1);
    ring_.assign(static_cast<size_t>(kCapacityFrames * channels_), 0);
    writeIndex_.store(0, std::memory_order_relaxed);
    oldestIndex_.store(0, std::memory_order_relaxed);
    anchorSeq_.store(0, std::memory_order_relaxed);
    anchorIndex_.store(0, std::memory_order_relaxed);
    anchorMicros_.store(0, std::memory_order_relaxed);
    haveAnchor_.store(false, std::memory_order_relaxed);
    playhead_.store(-1, std::memory_order_relaxed);
    targetDelay_.store(std::max(config_.minDelayMicros, videoDelay_), std::memory_order_relaxed);
    bucketStart_ = 0;
    bucket_ = 0;
    std::fill(std::begin(bucketMax_), std::end(bucketMax_), 0);
    playing_ = false;
    readIndex_ = 0;
    frac_ = 0.0;
    errorSmooth_ = 0.0;
    correcting_ = false;
    fadeIn_ = 0;
    statBuffered_.store(-1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
}

void AudioPlayout::setClockOffset(int64_t senderMinusLocalMicros) {
    offset_.store(senderMinusLocalMicros, std::memory_order_relaxed);
    offsetValid_.store(true, std::memory_order_release);
}

void AudioPlayout::setVideoDelay(int64_t micros) { videoDelay_ = std::max<int64_t>(micros, 0); }

void AudioPlayout::publishAnchor(int64_t index, int64_t micros) {
    const uint32_t seq = anchorSeq_.load(std::memory_order_relaxed);
    anchorSeq_.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    anchorIndex_.store(index, std::memory_order_relaxed);
    anchorMicros_.store(micros, std::memory_order_relaxed);
    anchorSeq_.store(seq + 2, std::memory_order_release);
    haveAnchor_.store(true, std::memory_order_release);
}

bool AudioPlayout::readAnchor(Anchor* out) const {
    if (!haveAnchor_.load(std::memory_order_acquire)) return false;
    // Seqlock read, bounded: the callback must not spin on a preempted writer (it then plays one
    // callback of silence).
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t s1 = anchorSeq_.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        out->index = anchorIndex_.load(std::memory_order_relaxed);
        out->micros = anchorMicros_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (anchorSeq_.load(std::memory_order_relaxed) == s1) return true;
    }
    return false;
}

void AudioPlayout::writeFrames(int64_t at, const void* samples, int frames) {
    const auto* src = static_cast<const uint8_t*>(samples);
    const size_t frameBytes = sizeof(int16_t) * static_cast<size_t>(channels_);
    int done = 0;
    while (done < frames) {
        const int64_t slot = (at + done) & kMask;
        const int n = static_cast<int>(std::min<int64_t>(frames - done, kCapacityFrames - slot));
        std::memcpy(&ring_[static_cast<size_t>(slot * channels_)], src + done * frameBytes, n * frameBytes);
        done += n;
    }
}

void AudioPlayout::writeSilence(int64_t at, int64_t frames) {
    frames = std::min(frames, kCapacityFrames);
    int64_t done = 0;
    while (done < frames) {
        const int64_t slot = (at + done) & kMask;
        const int64_t n = std::min(frames - done, kCapacityFrames - slot);
        std::fill_n(&ring_[static_cast<size_t>(slot * channels_)], static_cast<size_t>(n * channels_), int16_t{0});
        done += n;
    }
}

void AudioPlayout::updateTarget(int64_t captureMicros, int64_t arrivalMicros) {
    int64_t target = std::max(config_.minDelayMicros, videoDelay_);
    if (offsetValid_.load(std::memory_order_acquire)) {
        // Arrival delay of this chunk's first sample: capture (local clock) -> received.
        const int64_t transit = arrivalMicros - (captureMicros - offset_.load(std::memory_order_relaxed));
        if (bucketStart_ == 0 || arrivalMicros - bucketStart_ >= config_.jitterBucketMicros) {
            const int64_t steps = bucketStart_ == 0 ? 1 : (arrivalMicros - bucketStart_) / config_.jitterBucketMicros;
            for (int64_t i = 0; i < std::min<int64_t>(steps, kBuckets); ++i) {
                bucket_ = (bucket_ + 1) % kBuckets;
                bucketMax_[bucket_] = 0;
            }
            bucketStart_ = arrivalMicros;
        }
        bucketMax_[bucket_] = std::max(bucketMax_[bucket_], transit);
        const int64_t worst = *std::max_element(std::begin(bucketMax_), std::end(bucketMax_));
        target = std::max(target, worst + config_.marginMicros);
    }
    targetDelay_.store(std::min(target, config_.maxDelayMicros), std::memory_order_relaxed);
}

void AudioPlayout::push(int64_t captureMicros, const void* samples, int frames, int64_t arrivalMicros) {
    if (frames <= 0) return;
    frames = static_cast<int>(std::min<int64_t>(frames, kCapacityFrames / 2));
    updateTarget(captureMicros, arrivalMicros);

    int64_t w = writeIndex_.load(std::memory_order_relaxed);
    const int64_t durationMicros = int64_t{frames} * 1'000'000 / sampleRate_;
    Anchor anchor{};
    if (!readAnchor(&anchor)) {
        publishAnchor(w, captureMicros);
    } else {
        const int64_t expected = anchor.micros + (w - anchor.index) * 1'000'000 / sampleRate_;  // sender time of w
        const int64_t diff = captureMicros - expected;
        if (diff > config_.gapToleranceMicros) {
            // A hole (chunks dropped by the sender, or audio paused): silence until this chunk.
            const int64_t gapFrames = diff * sampleRate_ / 1'000'000;
            statGaps_.fetch_add(1, std::memory_order_relaxed);
            statGapMicros_.fetch_add(diff, std::memory_order_relaxed);
            if (gapFrames >= kCapacityFrames / 2) {
                // Longer than the buffer: start the timeline over here (the consumer re-syncs).
                w += gapFrames;
                oldestIndex_.store(w, std::memory_order_release);
            } else {
                writeSilence(w, gapFrames);
                w += gapFrames;
            }
            publishAnchor(w, captureMicros);
        } else if (diff < -config_.gapToleranceMicros) {
            if (captureMicros + durationMicros <= expected) {
                statDropped_.fetch_add(1, std::memory_order_relaxed);  // nothing new in it
                return;
            }
            publishAnchor(w, captureMicros);  // timestamps stepped back: follow them
        } else {
            // Contiguous: let this chunk's timestamp pull the anchor gently (timestamp jitter
            // averages out, real clock drift is followed).
            publishAnchor(w, expected + diff / 8);
        }
    }
    const int64_t playhead = playhead_.load(std::memory_order_relaxed);
    if (playhead >= 0 && w + frames <= playhead) statLate_.fetch_add(1, std::memory_order_relaxed);
    writeFrames(w, samples, frames);
    writeIndex_.store(w + frames, std::memory_order_release);
}

void AudioPlayout::render(int16_t* out, int frames, int64_t presentMicros) {
    const int ch = channels_;
    std::fill_n(out, static_cast<size_t>(frames * ch), int16_t{0});
    Anchor anchor{};
    if (frames <= 0 || !offsetValid_.load(std::memory_order_acquire) || !readAnchor(&anchor)) {
        statBuffered_.store(-1, std::memory_order_relaxed);
        return;
    }
    const int64_t w = writeIndex_.load(std::memory_order_acquire);
    const int64_t oldest = std::max(oldestIndex_.load(std::memory_order_acquire), w - kCapacityFrames + 4096);
    const int64_t offset = offset_.load(std::memory_order_relaxed);
    const int64_t target = targetDelay_.load(std::memory_order_relaxed);
    const double inRate = sampleRate_;
    const double baseStep = inRate / outputRate_;
    // Local time at which frame `i` is due at the speaker.
    auto dueMicros = [&](double i) {
        return static_cast<double>(anchor.micros - offset + target) + (i - static_cast<double>(anchor.index)) * 1e6 / inRate;
    };

    if (playing_ && readIndex_ < oldest) {
        playing_ = false;  // the timeline restarted or the ring was overrun
        statResyncs_.fetch_add(1, std::memory_order_relaxed);
    }
    if (!playing_) {
        // The frame due now, and enough after it for this callback.
        const double due = static_cast<double>(anchor.index) +
                           static_cast<double>(presentMicros + offset - target - anchor.micros) * inRate / 1e6;
        const int64_t start = std::llround(due);
        playhead_.store(start, std::memory_order_relaxed);
        const int64_t needed = start + static_cast<int64_t>(std::ceil(frames * baseStep)) + 2;
        // Before the oldest frame we have (stream start): its time has not come yet. Past what
        // has arrived: not here yet. Either way, silence for now.
        if (start < oldest || needed > w) {
            statBuffered_.store(std::max<int64_t>(w - start, 0) * 1'000'000 / sampleRate_, std::memory_order_relaxed);
            return;  // silence until the due audio has arrived
        }
        playing_ = true;
        readIndex_ = start;
        frac_ = 0.0;
        errorSmooth_ = 0.0;
        correcting_ = false;
        fadeIn_ = config_.fadeFrames;
    }

    // Positive error: the current frame will be heard later than due -> play faster.
    const double error = static_cast<double>(presentMicros) - dueMicros(static_cast<double>(readIndex_) + frac_);
    const double callbackSeconds = frames / static_cast<double>(outputRate_);
    const double alpha = std::min(1.0, callbackSeconds / config_.errorSmoothingSeconds);
    errorSmooth_ += alpha * (error - errorSmooth_);
    bool jump = std::fabs(error) > static_cast<double>(config_.jumpMicros);
    if (!correcting_ && std::fabs(errorSmooth_) > static_cast<double>(config_.correctStartMicros)) {
        correcting_ = true;
        statCorrections_.fetch_add(1, std::memory_order_relaxed);
    } else if (correcting_ && std::fabs(errorSmooth_) < static_cast<double>(config_.correctStopMicros)) {
        correcting_ = false;
    }
    const double ratio =
        correcting_ ? std::clamp(errorSmooth_ / 1e6 * config_.gainPerSecond, -config_.maxRatio, config_.maxRatio) : 0.0;
    const double step = jump ? baseStep : baseStep * (1.0 + ratio);

    // Frames playable before running out (interpolation reads index + 1).
    const double readable = static_cast<double>(w - 1 - readIndex_) - frac_;
    const int available = readable <= 0 ? 0 : static_cast<int>(std::min<double>(frames, std::floor(readable / step) + 1));
    const bool underrun = available < frames;
    // Fade out before a jump or an underrun so neither clicks.
    const int fadeOutLength = jump ? std::min(config_.fadeFrames, frames) : underrun ? std::min(config_.fadeFrames, available) : 0;
    const int fadeOutStart = (jump ? frames : available) - fadeOutLength;

    const int64_t startIndex = readIndex_;
    for (int k = 0; k < available; ++k) {
        const int64_t i0 = readIndex_ & kMask;
        const int64_t i1 = (readIndex_ + 1) & kMask;
        float gain = 1.0f;
        if (fadeIn_ > 0) {
            gain = 1.0f - static_cast<float>(fadeIn_) / static_cast<float>(config_.fadeFrames + 1);
            --fadeIn_;
        }
        if (fadeOutLength > 0 && k >= fadeOutStart) {
            gain *= static_cast<float>(fadeOutStart + fadeOutLength - k) / static_cast<float>(fadeOutLength + 1);
        }
        for (int c = 0; c < ch; ++c) {
            const float a = ring_[static_cast<size_t>(i0 * ch + c)];
            const float b = ring_[static_cast<size_t>(i1 * ch + c)];
            const float v = (a + (b - a) * static_cast<float>(frac_)) * gain;
            out[k * ch + c] = static_cast<int16_t>(std::lrint(std::clamp(v, -32768.0f, 32767.0f)));
        }
        frac_ += step;
        const double whole = std::floor(frac_);
        readIndex_ += static_cast<int64_t>(whole);
        frac_ -= whole;
    }
    // The producer may have lapped what was just read (consumer stalled > 1.3 s): discard it.
    if (writeIndex_.load(std::memory_order_acquire) - kCapacityFrames > startIndex) {
        std::fill_n(out, static_cast<size_t>(frames * ch), int16_t{0});
        jump = true;
    }

    const int64_t latency = static_cast<int64_t>(static_cast<double>(presentMicros) -
                                                 (dueMicros(static_cast<double>(startIndex)) - static_cast<double>(target)));
    statLatencySum_.fetch_add(latency, std::memory_order_relaxed);
    statLatencySamples_.fetch_add(1, std::memory_order_relaxed);
    atomicMax(statLatencyMax_, latency);
    statBuffered_.store(std::max<int64_t>(w - readIndex_, 0) * 1'000'000 / sampleRate_, std::memory_order_relaxed);
    if (ratio != 0.0 && !jump) {
        statCorrectedNanos_.fetch_add(static_cast<int64_t>(available * (step - baseStep) / inRate * 1e9),
                                      std::memory_order_relaxed);
    }
    playhead_.store(readIndex_, std::memory_order_relaxed);
    if (underrun) statUnderruns_.fetch_add(1, std::memory_order_relaxed);
    if (jump) statResyncs_.fetch_add(1, std::memory_order_relaxed);
    if (underrun || jump) playing_ = false;
}

AudioPlayoutStats AudioPlayout::takeStats() {
    AudioPlayoutStats s;
    s.bufferedMicros = statBuffered_.load(std::memory_order_relaxed);
    s.targetDelayMicros = targetDelay_.load(std::memory_order_relaxed);
    s.latencySumMicros = statLatencySum_.exchange(0, std::memory_order_relaxed);
    s.latencyMaxMicros = statLatencyMax_.exchange(0, std::memory_order_relaxed);
    s.latencySamples = statLatencySamples_.exchange(0, std::memory_order_relaxed);
    s.underruns = statUnderruns_.exchange(0, std::memory_order_relaxed);
    s.resyncs = statResyncs_.exchange(0, std::memory_order_relaxed);
    s.lateChunks = statLate_.exchange(0, std::memory_order_relaxed);
    s.droppedChunks = statDropped_.exchange(0, std::memory_order_relaxed);
    s.gaps = statGaps_.exchange(0, std::memory_order_relaxed);
    s.gapMicros = statGapMicros_.exchange(0, std::memory_order_relaxed);
    s.corrections = statCorrections_.exchange(0, std::memory_order_relaxed);
    s.correctedMicros = statCorrectedNanos_.exchange(0, std::memory_order_relaxed) / 1000;
    return s;
}

}  // namespace qm
