#pragma once

#include "audio_playout.h"

#include <aaudio/AAudio.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace qm {

// AAudio output for the Mac's audio: a low-latency, shared, s16 stream at the source's rate and
// channel count whose data callback pulls from an AudioPlayout. The callback learns when its
// frames will be heard from AAudioStream_getTimestamp, which the owner's thread refreshes
// (service()) and publishes to it; the callback itself makes no system calls and takes no locks.
// Uses the media volume (the headset's volume buttons work as usual).
//
// open/close/service: one owner thread (the network thread).
class AudioOutput {
public:
    explicit AudioOutput(AudioPlayout& playout) : playout_(playout) {}
    ~AudioOutput() { close(); }
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // Resets the playout for this format and starts the stream. False (logged) if AAudio refuses;
    // service() then retries once a second until close().
    bool open(int sampleRate, int channels);
    void close();
    bool isOpen() const { return stream_ != nullptr; }
    // Refreshes the presentation timestamp (about every 50 ms), reopens the stream after a device
    // change or error reported by the error callback (e.g. Bluetooth headphones connected), and
    // retries a failed open.
    void service(int64_t nowMicros);
    // For the stats line: "AAudio 48000 Hz 2 ch, device buffer 4.0 ms, device xruns 0".
    std::string describe();

private:
    static aaudio_data_callback_result_t dataCallback(AAudioStream* stream, void* user, void* audioData,
                                                      int32_t numFrames);
    static void errorCallback(AAudioStream* stream, void* user, aaudio_result_t error);
    bool openStream();
    void closeStream();
    void refreshTimestamp();

    AudioPlayout& playout_;
    AAudioStream* stream_ = nullptr;
    bool wanted_ = false;       // open() called, close() not: keep (re)opening
    int sampleRate_ = 0;        // source format
    int channels_ = 0;
    int deviceRate_ = 0;        // what the device opened at
    int64_t lastAttemptMicros_ = 0;
    int64_t lastTimestampMicros_ = 0;
    static constexpr int64_t kRetryMicros = 1'000'000;
    int32_t lastXRuns_ = 0;

    // Callback state.
    int64_t framesWritten_ = 0;
    // Latest timestamp: frame `tsFrame_` is heard at `tsNanos_` (CLOCK_MONOTONIC), seqlock.
    std::atomic<uint32_t> tsSeq_{0};
    std::atomic<int64_t> tsFrame_{0};
    std::atomic<int64_t> tsNanos_{0};
    std::atomic<bool> tsValid_{false};
    std::atomic<int32_t> bufferFrames_{0};
    std::atomic<bool> restartNeeded_{false};
    std::atomic<int32_t> lastError_{0};
};

}  // namespace qm
