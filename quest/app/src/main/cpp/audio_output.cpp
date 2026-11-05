#include "audio_output.h"

#include "log.h"

#include <cstdio>
#include <ctime>

namespace qm {
namespace {

constexpr int64_t kTimestampIntervalMicros = 50'000;

int64_t monotonicNanos() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t{ts.tv_sec} * 1'000'000'000 + ts.tv_nsec;
}

const char* performanceModeName(aaudio_performance_mode_t mode) {
    switch (mode) {
        case AAUDIO_PERFORMANCE_MODE_LOW_LATENCY: return "low-latency";
        case AAUDIO_PERFORMANCE_MODE_POWER_SAVING: return "power-saving";
        default: return "none";
    }
}

}  // namespace

bool AudioOutput::open(int sampleRate, int channels) {
    close();
    sampleRate_ = sampleRate;
    channels_ = channels;
    wanted_ = true;
    return openStream();
}

void AudioOutput::close() {
    wanted_ = false;
    closeStream();
}

bool AudioOutput::openStream() {
    lastAttemptMicros_ = monotonicNanos() / 1000;
    const int sampleRate = sampleRate_, channels = channels_;
    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t r = AAudio_createStreamBuilder(&builder);
    if (r != AAUDIO_OK) {
        LOGE("Audio: AAudio_createStreamBuilder: %s", AAudio_convertResultToText(r));
        return false;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, channels);
    AAudioStreamBuilder_setSampleRate(builder, sampleRate);
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MOVIE);
    AAudioStreamBuilder_setDataCallback(builder, dataCallback, this);
    AAudioStreamBuilder_setErrorCallback(builder, errorCallback, this);
    r = AAudioStreamBuilder_openStream(builder, &stream_);
    AAudioStreamBuilder_delete(builder);
    if (r != AAUDIO_OK) {
        LOGE("Audio: AAudioStreamBuilder_openStream(%d Hz, %d ch): %s", sampleRate, channels,
             AAudio_convertResultToText(r));
        stream_ = nullptr;
        return false;
    }
    if (AAudioStream_getChannelCount(stream_) != channels || AAudioStream_getFormat(stream_) != AAUDIO_FORMAT_PCM_I16) {
        LOGE("Audio: device opened %d ch format %d instead of %d ch s16", AAudioStream_getChannelCount(stream_),
             AAudioStream_getFormat(stream_), channels);
        closeStream();
        return false;
    }
    deviceRate_ = AAudioStream_getSampleRate(stream_);
    // Two bursts of device buffering: the least that stays glitch-free with a light callback.
    const int32_t burst = AAudioStream_getFramesPerBurst(stream_);
    const int32_t buffer = AAudioStream_setBufferSizeInFrames(stream_, 2 * burst);
    bufferFrames_ = buffer > 0 ? buffer : AAudioStream_getBufferSizeInFrames(stream_);
    playout_.reset(sampleRate, channels, deviceRate_);
    framesWritten_ = 0;
    tsValid_ = false;
    restartNeeded_ = false;
    lastTimestampMicros_ = 0;
    lastXRuns_ = 0;
    r = AAudioStream_requestStart(stream_);
    if (r != AAUDIO_OK) {
        LOGE("Audio: AAudioStream_requestStart: %s", AAudio_convertResultToText(r));
        closeStream();
        return false;
    }
    LOGI("Audio output started: AAudio %d Hz%s, %d ch, burst %d, buffer %d frames (%.1f ms), %s, %s", deviceRate_,
         deviceRate_ != sampleRate ? " (source rate refused: resampled linearly)" : "", channels, burst,
         bufferFrames_.load(), 1000.0 * bufferFrames_.load() / deviceRate_,
         performanceModeName(AAudioStream_getPerformanceMode(stream_)),
         AAudioStream_getSharingMode(stream_) == AAUDIO_SHARING_MODE_EXCLUSIVE ? "exclusive" : "shared");
    return true;
}

void AudioOutput::closeStream() {
    if (stream_ == nullptr) return;
    AAudioStream_close(stream_);  // stops it; returns once the callback has finished
    stream_ = nullptr;
    tsValid_ = false;
    LOGI("Audio output closed");
}

void AudioOutput::service(int64_t nowMicros) {
    if (stream_ != nullptr && restartNeeded_) {
        LOGW("Audio: stream error %s (device change?); reopening", AAudio_convertResultToText(lastError_.load()));
        closeStream();
        openStream();
        return;
    }
    if (stream_ == nullptr) {
        // Wanted but failed to open (or to reopen): retry once a second.
        if (wanted_ && nowMicros - lastAttemptMicros_ >= kRetryMicros) openStream();
        return;
    }
    if (nowMicros - lastTimestampMicros_ >= kTimestampIntervalMicros) {
        lastTimestampMicros_ = nowMicros;
        refreshTimestamp();
    }
}

void AudioOutput::refreshTimestamp() {
    int64_t frame = 0, nanos = 0;
    if (AAudioStream_getTimestamp(stream_, CLOCK_MONOTONIC, &frame, &nanos) != AAUDIO_OK) return;
    const uint32_t seq = tsSeq_.load(std::memory_order_relaxed);
    tsSeq_.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    tsFrame_.store(frame, std::memory_order_relaxed);
    tsNanos_.store(nanos, std::memory_order_relaxed);
    tsSeq_.store(seq + 2, std::memory_order_release);
    tsValid_.store(true, std::memory_order_release);
}

std::string AudioOutput::describe() {
    if (stream_ == nullptr) return "no audio output";
    const int32_t xruns = AAudioStream_getXRunCount(stream_);
    const int32_t newXRuns = xruns >= lastXRuns_ ? xruns - lastXRuns_ : xruns;
    lastXRuns_ = xruns;
    char text[160];
    std::snprintf(text, sizeof(text), "AAudio %d Hz %d ch, device buffer %.1f ms, device xruns %d", deviceRate_,
                  channels_, 1000.0 * bufferFrames_.load() / deviceRate_, newXRuns);
    return text;
}

aaudio_data_callback_result_t AudioOutput::dataCallback(AAudioStream* /*stream*/, void* user, void* audioData,
                                                        int32_t numFrames) {
    auto* self = static_cast<AudioOutput*>(user);
    int64_t presentNanos = 0;
    bool fromTimestamp = false;
    if (self->tsValid_.load(std::memory_order_acquire)) {
        // Seqlock read; a writer preempted mid-update just means the estimate below this once.
        for (int attempt = 0; attempt < 4 && !fromTimestamp; ++attempt) {
            const uint32_t s1 = self->tsSeq_.load(std::memory_order_acquire);
            if (s1 & 1u) continue;
            const int64_t frame = self->tsFrame_.load(std::memory_order_relaxed);
            const int64_t nanos = self->tsNanos_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (self->tsSeq_.load(std::memory_order_relaxed) != s1) continue;
            presentNanos = nanos + (self->framesWritten_ - frame) * 1'000'000'000 / self->deviceRate_;
            fromTimestamp = true;
        }
    }
    const int64_t nowNanos = monotonicNanos();
    if (!fromTimestamp) {
        // No timestamp yet (just started): about one device buffer from now.
        presentNanos = nowNanos + int64_t{self->bufferFrames_.load()} * 1'000'000'000 / self->deviceRate_;
    }
    self->playout_.render(static_cast<int16_t*>(audioData), numFrames, presentNanos / 1000, nowNanos / 1000);
    self->framesWritten_ += numFrames;
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AudioOutput::errorCallback(AAudioStream* /*stream*/, void* user, aaudio_result_t error) {
    // Runs on its own thread; the stream may only be reopened from the owner (service()).
    auto* self = static_cast<AudioOutput*>(user);
    self->lastError_ = error;
    self->restartNeeded_ = true;
}

}  // namespace qm
