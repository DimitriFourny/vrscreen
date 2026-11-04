#pragma once

#include "stream_client.h"

#include <android/native_window.h>
#include <media/NdkMediaCodec.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace qm {

// AMediaCodec decoding straight into an ANativeWindow (the screen quad's Surface).
// Input is queued from the network thread; a dedicated output thread renders every decoded
// picture immediately (no B-frames in the stream, so decode order = display order).
class VideoDecoder {
public:
    enum class QueueResult { Queued, NoInputBuffer, Error };

    // Counters since the previous takeStats().
    struct Stats {
        uint64_t rendered = 0;
        uint64_t latencySamples = 0;
        int64_t latencySumMicros = 0;  // sum of (output client time - capture sender time)
        int64_t latencyMaxMicros = 0;  // add the clock offset to get capture -> output latency
        uint64_t decodeSamples = 0;
        int64_t decodeSumMicros = 0;  // queueInputBuffer -> output buffer available
        int64_t decodeMaxMicros = 0;
    };

    VideoDecoder() = default;
    ~VideoDecoder() { stop(); }
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool start(const StreamConfig& config, ANativeWindow* window, std::string* error);
    // After stop() returns the codec is released and nothing writes to the window any more.
    void stop();
    QueueResult queue(const EncodedFrame& frame, int64_t timeoutMicros);

    bool running() const { return running_.load(); }
    // Pictures rendered since start(); > 0 means the window shows a decoded picture.
    uint64_t renderedTotal() const { return renderedTotal_.load(); }
    uint64_t queuedTotal() const { return queuedTotal_.load(); }
    const std::string& name() const { return name_; }
    Stats takeStats();

private:
    void outputLoop();

    AMediaCodec* codec_ = nullptr;
    std::string name_;
    std::thread outputThread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> renderedTotal_{0};
    std::atomic<uint64_t> queuedTotal_{0};
    std::atomic<uint64_t> statRendered_{0};
    std::atomic<uint64_t> statLatencySamples_{0};
    std::atomic<int64_t> statLatencySum_{0};
    std::atomic<int64_t> statLatencyMax_{INT64_MIN};

    // Input queue times by presentation time, to measure the decoder's own latency.
    struct QueuedInput {
        int64_t pts = -1;
        int64_t queuedAt = 0;
    };
    std::mutex queuedMutex_;
    std::array<QueuedInput, 32> queued_{};  // guarded by queuedMutex_, as are the next four
    size_t queuedNext_ = 0;
    uint64_t statDecodeSamples_ = 0;
    int64_t statDecodeSum_ = 0;
    int64_t statDecodeMax_ = 0;
};

}  // namespace qm
