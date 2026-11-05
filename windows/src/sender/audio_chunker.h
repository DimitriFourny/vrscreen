// Re-chunks captured audio buffers of any size into fixed 10 ms chunks of s16le interleaved PCM,
// each stamped with the capture time of its first sample (port of the Mac AudioChunker):
// - a buffer that continues the previous one (within `gapToleranceMicros`) extends the pending
//   samples, re-timed so the new buffer's first frame lands on its timestamp;
// - a buffer further away (a gap, or time going backwards) drops the pending partial chunk and
//   starts over at the new timestamp.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "protocol.h"

namespace vr {

inline int16_t pcm16(float v) {
    const float scaled = std::nearbyint(v * 32767.0f);
    if (!(scaled == scaled)) return 0;  // NaN
    if (scaled >= 32767.0f) return 32767;
    if (scaled <= -32768.0f) return -32768;
    return int16_t(scaled);
}

class AudioChunker {
public:
    AudioChunker(int sampleRate, int channels, int framesPerChunk, double gapToleranceMicros = 5000)
        : sampleRate_(sampleRate), channels_(channels), framesPerChunk_(framesPerChunk),
          gapToleranceMicros_(gapToleranceMicros) {}

    int sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }
    int framesPerChunk() const { return framesPerChunk_; }
    long long discardedFrames() const { return discarded_; }
    int pendingFrames() const { return int(pending_.size()) / channels_; }
    void reset() { pending_.clear(); }

    // `sample(frame, channel)` returns a float sample (nominal -1...1).
    template <typename SampleFn>
    std::vector<proto::AudioMessage> append(int frameCount, uint64_t captureMicros, SampleFn&& sample) {
        std::vector<proto::AudioMessage> chunks;
        if (frameCount <= 0) return chunks;
        const double microsPerFrame = 1e6 / sampleRate_;
        const double start = double(captureMicros);
        if (pending_.empty()) {
            pendingStart_ = start;
        } else {
            const double expected = pendingStart_ + pendingFrames() * microsPerFrame;
            if (std::fabs(start - expected) > gapToleranceMicros_) {
                discarded_ += pendingFrames();
                pending_.clear();
                pendingStart_ = start;
            } else {
                pendingStart_ = start - pendingFrames() * microsPerFrame;
            }
        }
        pending_.reserve(pending_.size() + size_t(frameCount) * size_t(channels_));
        for (int f = 0; f < frameCount; ++f) {
            for (int c = 0; c < channels_; ++c) pending_.push_back(pcm16(sample(f, c)));
        }
        const size_t samplesPerChunk = size_t(framesPerChunk_) * size_t(channels_);
        size_t consumed = 0;
        while (pending_.size() - consumed >= samplesPerChunk) {
            proto::AudioMessage m;
            m.pcm.resize(samplesPerChunk * 2);
            for (size_t i = 0; i < samplesPerChunk; ++i) {
                const uint16_t v = uint16_t(pending_[consumed + i]);
                m.pcm[2 * i] = uint8_t(v & 0xFF);  // little-endian samples
                m.pcm[2 * i + 1] = uint8_t(v >> 8);
            }
            const double micros = std::nearbyint(pendingStart_ + double(consumed / channels_) * microsPerFrame);
            m.captureMicros = micros > 0 ? uint64_t(micros) : 0;
            chunks.push_back(std::move(m));
            consumed += samplesPerChunk;
        }
        if (consumed > 0) {
            pendingStart_ += double(consumed / channels_) * microsPerFrame;
            pending_.erase(pending_.begin(), pending_.begin() + ptrdiff_t(consumed));
        }
        return chunks;
    }

private:
    int sampleRate_;
    int channels_;
    int framesPerChunk_;
    double gapToleranceMicros_;
    std::vector<int16_t> pending_;
    double pendingStart_ = 0;
    long long discarded_ = 0;
};

// Stereo downmix weights for a WASAPI channel mask (SPEAKER_* bits): for each source channel
// the gain into left and right. Mono goes to both sides; 5.1/7.1 use the usual -3 dB center and
// surround mix; LFE is dropped.
struct DownmixMatrix {
    std::vector<float> left, right;
};
DownmixMatrix makeDownmix(int channels, uint32_t channelMask);

}  // namespace vr
