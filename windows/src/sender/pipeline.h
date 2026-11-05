// Capture → encode → send, with backpressure that never drops encoded frames (port of the Mac
// SenderPipeline): while the client's window is full (unsent socket backlog, or for clients that
// send FRAME_ACK, frames not yet consumed) or the encoder is busy, new captures are not encoded;
// only the latest is kept, and it is encoded as soon as there is room, so the final state of a
// now-static screen is never lost. Methods run on the event loop thread; encoding runs on its
// own thread.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "encoder.h"
#include "event_loop.h"
#include "server.h"

namespace vr {

class SenderPipeline {
public:
    static constexpr int kMaxUnsentFrames = 2;
    static constexpr int kMaxEncodingFrames = 1;

    SenderPipeline(EventLoop& loop, MirrorServer& server, EncoderSettings settings);
    ~SenderPipeline();

    void handleCapture(VideoFrame frame);
    // Codec, bitrate or frame rate changed: the next frame goes to a new encoder (keyframe, and a
    // CONFIG if the format changed).
    void replaceEncoder(EncoderSettings settings);
    void discardLatestCapture() { latest_.reset(); }
    void stop();
    // One stats line per second; `audioText` describes the audio side.
    void logStats(const std::string& audioText);

    // Called with the error when encoding starts failing, and with nullptr when it recovers.
    std::function<void(const std::string* error)> onEncoderError;

private:
    struct Capture {
        VideoFrame frame;
        uint64_t sequence = 0;
    };
    struct EncodeResult {
        bool ok = false;
        std::string error;
        std::optional<EncodedOutput> output;
    };
    void pump();
    void handleEncoded(EncodeResult result, const std::shared_ptr<ClientSession>& client);
    void setEncoderError(const std::string* error);
    void runEncoderThread();
    void postJob(std::function<void(VideoEncoder&)> job);

    EventLoop& loop_;
    MirrorServer& server_;
    std::optional<Capture> latest_;
    uint64_t captureSequence_ = 0;
    uint64_t lastEncodedSequence_ = 0;
    int framesEncoding_ = 0;
    std::optional<std::string> encoderError_;
    const void* preparedGpu_ = nullptr;
    int preparedW_ = 0, preparedH_ = 0;

    // Encoder thread.
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void(VideoEncoder&)>> jobs_;
    bool stopping_ = false;
    std::unique_ptr<VideoEncoder> encoder_;  // encoder thread only (after construction)

    struct Stats {
        int captured = 0, encoded = 0, skipped = 0, keyframes = 0;
        uint32_t maxInFlight = 0;
        double encodeMsTotal = 0, encodeMsMax = 0;
    } stats_;
    long long totalSkipped_ = 0;
    uint64_t lastStatsMicros_ = 0;
    long long lastBytes_ = 0, lastAudioQueued_ = 0, lastAudioDropped_ = 0;
};

}  // namespace vr
