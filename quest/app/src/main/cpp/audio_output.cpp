#include "audio_output.h"

#include "log.h"

#include <sys/system_properties.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

namespace qm {
namespace {

constexpr int64_t kTimestampIntervalMicros = 50'000;
constexpr int kMaxBufferBursts = 4;
// A/B switches: "shared" forces the shared mixer path; 1..8 sets the starting buffer in bursts.
constexpr const char* kShareProperty = "debug.vrscreen.audioshare";
constexpr const char* kBurstsProperty = "debug.vrscreen.audiobursts";

std::string readProperty(const char* name) {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get(name, value);
    return value;
}

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
    // Exclusive (MMAP straight to the device, no mixer) has the lowest latency; the system may
    // refuse it (busy, Bluetooth...) or grant shared instead, which openWith() logs.
    if (readProperty(kShareProperty) != "shared" && openWith(AAUDIO_SHARING_MODE_EXCLUSIVE)) return true;
    return openWith(AAUDIO_SHARING_MODE_SHARED);
}

bool AudioOutput::openWith(aaudio_sharing_mode_t sharing) {
    const int sampleRate = sampleRate_, channels = channels_;
    const char* wanted = sharing == AAUDIO_SHARING_MODE_EXCLUSIVE ? "exclusive" : "shared";
    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t r = AAudio_createStreamBuilder(&builder);
    if (r != AAUDIO_OK) {
        LOGE("Audio: AAudio_createStreamBuilder: %s", AAudio_convertResultToText(r));
        return false;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, sharing);
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
        LOGW("Audio: AAudioStreamBuilder_openStream(%s, %d Hz, %d ch): %s", wanted, sampleRate, channels,
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
    exclusive_ = AAudioStream_getSharingMode(stream_) == AAUDIO_SHARING_MODE_EXCLUSIVE;
    // The device buffer: 1 burst when exclusive, 2 through the shared mixer, one more burst each
    // time the device reports xruns (service()), up to kMaxBufferBursts.
    burst_ = AAudioStream_getFramesPerBurst(stream_);
    const int requested = std::atoi(readProperty(kBurstsProperty).c_str());
    bufferBursts_ = requested >= 1 && requested <= 8 ? requested : exclusive_ ? 1 : 2;
    applyBufferSize();
    playout_.reset(sampleRate, channels, deviceRate_);
    framesWritten_ = 0;
    tsValid_ = false;
    restartNeeded_ = false;
    lastTimestampMicros_ = 0;
    lastXRuns_ = 0;
    tunedXRuns_ = 0;
    r = AAudioStream_requestStart(stream_);
    if (r != AAUDIO_OK) {
        LOGW("Audio: AAudioStream_requestStart (%s): %s", wanted, AAudio_convertResultToText(r));
        closeStream();
        return false;
    }
    LOGI("Audio output started: AAudio %d Hz%s, %d ch, %s (asked %s), %s, burst %d, buffer %d bursts = %d frames "
         "(%.1f ms), capacity %d frames",
         deviceRate_, deviceRate_ != sampleRate ? " (source rate refused: resampled linearly)" : "", channels,
         exclusive_ ? "exclusive" : "shared", wanted, performanceModeName(AAudioStream_getPerformanceMode(stream_)),
         burst_, bufferBursts_, bufferFrames_.load(), 1000.0 * bufferFrames_.load() / deviceRate_,
         AAudioStream_getBufferCapacityInFrames(stream_));
    return true;
}

void AudioOutput::applyBufferSize() {
    const int32_t size = AAudioStream_setBufferSizeInFrames(stream_, bufferBursts_ * burst_);
    bufferFrames_ = size > 0 ? size : AAudioStream_getBufferSizeInFrames(stream_);
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
        // The device ran dry (our callback was late): one more burst of buffering, the usual
        // latency tuning, so it settles at the smallest buffer that holds.
        const int32_t xruns = AAudioStream_getXRunCount(stream_);
        if (xruns > tunedXRuns_) {
            tunedXRuns_ = xruns;
            if (bufferBursts_ < kMaxBufferBursts) {
                ++bufferBursts_;
                applyBufferSize();
                LOGW("Audio: device xruns (%d so far): buffer now %d bursts (%.1f ms)", xruns, bufferBursts_,
                     1000.0 * bufferFrames_.load() / deviceRate_);
            }
        }
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
    std::snprintf(text, sizeof(text), "AAudio %d Hz %d ch %s, device buffer %d x %d frames (%.1f ms), device xruns %d",
                  deviceRate_, channels_, exclusive_ ? "exclusive" : "shared", bufferBursts_, burst_,
                  1000.0 * bufferFrames_.load() / deviceRate_, newXRuns);
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
