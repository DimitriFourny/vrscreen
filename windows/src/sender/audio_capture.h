// The PC's sound → headset.
// - AudioSink: re-chunks float stereo audio into 10 ms s16le AUDIO messages and hands them to the
//   server on the event loop (any thread may push).
// - LoopbackCapture: WASAPI loopback of the default output device on its own thread, stamped with
//   the QPC time of each buffer's first frame (the same clock as the video). A silent render
//   stream keeps the loopback flowing while nothing plays, so the timeline has no gaps. Follows
//   default-device changes. Optionally mutes the PC's output while audio goes to a headset.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio_chunker.h"
#include "event_loop.h"
#include "server.h"

namespace vr {

class AudioSink {
public:
    AudioSink(EventLoop& loop, MirrorServer& server) : loop_(loop), server_(server) {}
    // Loop thread: audio on/off (AUDIO_CONFIG for clients; off drops the pending partial chunk).
    void setEnabled(bool on);
    bool enabled() const { return enabled_; }
    // Any thread: interleaved stereo float, first frame captured at `micros`.
    void push(const float* stereo, int frames, int sampleRate, uint64_t micros);
    // Any thread: how late buffers arrived (now − end of buffer), since the last call.
    void noteDelivery(double ageMs);
    std::string takeStats();

private:
    EventLoop& loop_;
    MirrorServer& server_;
    std::atomic<bool> enabled_{false};
    std::mutex mutex_;
    std::unique_ptr<AudioChunker> chunker_;
    proto::AudioConfigMessage config_;
    std::vector<double> ages_;
};

class LoopbackCapture {
public:
    LoopbackCapture(AudioSink& sink, int offsetMs) : sink_(sink), offsetMicros_(uint64_t(offsetMs) * 1000) {}
    ~LoopbackCapture() { stop(); }
    void start();
    void stop();
    // Mute the default output while true (only if it was not muted already; restored after).
    void setMute(bool mute) { wantMute_ = mute; }
    // Called on the capture thread with an error, or an empty string once capture runs.
    std::function<void(const std::string&)> onStatus;

private:
    void run();
    bool runDevice(std::string& error);
    AudioSink& sink_;
    uint64_t offsetMicros_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> wantMute_{false};
};

}  // namespace vr
