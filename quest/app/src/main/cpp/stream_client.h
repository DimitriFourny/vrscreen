#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace qm {

// Wire protocol v1 (docs/protocol.md).
enum class Codec : uint8_t { H264 = 1, HEVC = 2 };

struct StreamConfig {
    Codec codec = Codec::HEVC;
    int32_t width = 0;
    int32_t height = 0;
    std::vector<uint8_t> parameterSets;  // Annex-B VPS/SPS/PPS (HEVC) or SPS/PPS (H.264)
    std::vector<uint8_t> payload;        // raw CONFIG payload, to detect identical CONFIGs

    bool valid() const { return width > 0 && height > 0; }
};

struct EncodedFrame {
    bool keyframe = false;
    uint64_t captureTimeMicros = 0;  // sender clock
    const uint8_t* data = nullptr;   // Annex-B access unit
    size_t size = 0;
};

// Link counters since the previous stats tick.
struct LinkStats {
    double seconds = 0;
    uint64_t frames = 0;
    uint64_t bytes = 0;
    int64_t rttMicros = -1;  // best RTT in the offset window, -1 if unknown
};

// Callbacks, all on the network thread.
class StreamListener {
public:
    virtual ~StreamListener() = default;
    virtual void onConnecting(const std::string& endpoint, const std::string& lastError) = 0;
    virtual void onConnected(const std::string& endpoint) = 0;
    // Return false to drop the connection (the client reconnects after 1 s).
    virtual bool onConfig(const StreamConfig& config) = 0;
    // Called for every FRAME. When it returns, the frame counts as consumed (handed to the decoder
    // or deliberately discarded) and is acknowledged with FRAME_ACK. Return false to reconnect.
    virtual bool onFrame(const EncodedFrame& frame) = 0;
    virtual void onDisconnected(const std::string& reason) = 0;
    // About once per second while connected.
    virtual void onStatsTick(const LinkStats& stats) = 0;
};

// TCP client for the Mac sender: connects to debug.questmirror.host/port (default
// 127.0.0.1:7777, i.e. `adb reverse tcp:7777 tcp:7777`), validates the hello, parses messages,
// pings for the clock offset, acknowledges frames and reconnects every second.
class StreamClient {
public:
    explicit StreamClient(StreamListener& listener) : listener_(listener) {}
    ~StreamClient() { stop(); }
    StreamClient(const StreamClient&) = delete;
    StreamClient& operator=(const StreamClient&) = delete;

    void start();
    // Closes the connection and joins the network thread.
    void stop();

    // CLOCK_MONOTONIC in microseconds (the clock used for PING).
    static int64_t nowMicros();
    // Sender clock minus client clock, from the lowest-RTT PING of the last 16. False until known.
    bool clockOffset(int64_t* offsetMicros) const;

private:
    void run();
    std::string session(int fd, const std::string& endpoint);
    bool handleMessage(uint8_t type, const uint8_t* payload, uint32_t length, std::string* error);
    bool sendPing(int fd);
    bool sendAck(int fd);
    void onPong(const uint8_t* payload, uint32_t length);
    bool sleepInterruptible(int milliseconds);

    StreamListener& listener_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    std::atomic<int> fd_{-1};

    // Per connection (network thread).
    uint32_t framesConsumed_ = 0;
    uint32_t framesAcked_ = 0;
    uint64_t statFrames_ = 0;
    uint64_t statBytes_ = 0;
    struct ClockSample {
        int64_t rtt;
        int64_t offset;
    };
    std::deque<ClockSample> clockSamples_;
    std::atomic<bool> offsetValid_{false};
    std::atomic<int64_t> offset_{0};
    std::atomic<int64_t> bestRtt_{-1};
};

}  // namespace qm
