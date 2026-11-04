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

#include "screen_source.h"

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
    // captureTimeMicros is newer than every earlier frame on this connection. False for keyframes
    // re-encoded from an old capture (on connect, or the periodic keyframe after a static pause),
    // which must not count towards latency.
    bool freshCapture = false;
    const uint8_t* data = nullptr;  // Annex-B access unit
    size_t size = 0;
};

// Link counters since the previous stats tick.
struct LinkStats {
    double seconds = 0;
    uint64_t frames = 0;
    uint64_t bytes = 0;
    int64_t rttMicros = -1;  // best RTT in the offset window, -1 if unknown
};

// Where the client looks for the sender, for the status panel.
struct SearchState {
    std::string overrideEndpoint;  // "host:port" from debug.vrscreen.host, empty = automatic
    std::string usbResult;         // outcome of the last USB (127.0.0.1) attempt, empty = not tried
    std::string wifiResult;        // outcome of the last discovery / Wi-Fi attempt
    std::string lastDisconnect;    // why the previous connection ended, if any
};

// Callbacks, all on the network thread.
class StreamListener {
public:
    virtual ~StreamListener() = default;
    virtual void onSearching(const SearchState& state) = 0;
    // `transport`: "USB (adb reverse)", "Wi-Fi 192.168.1.10 (name)" or "192.168.1.10:7777 (override)".
    virtual void onConnected(const std::string& transport) = 0;
    // Return false to drop the connection (the client reconnects).
    virtual bool onConfig(const StreamConfig& config) = 0;
    // Called for every FRAME. When it returns, the frame counts as consumed (handed to the decoder
    // or deliberately discarded) and is acknowledged with FRAME_ACK. Return false to reconnect.
    virtual bool onFrame(const EncodedFrame& frame) = 0;
    // CONTROL message from the Mac (shortcuts, menu-bar icon): move, resize or restyle the screen.
    virtual void onControl(const ScreenCommand& command) = 0;
    virtual void onDisconnected(const std::string& reason) = 0;
    // About once per second while connected. Return false to reconnect (e.g. the decoder died).
    virtual bool onStatsTick(const LinkStats& stats) = 0;
};

// Client for the Mac sender.
//
// Connection policy (re-run after every disconnect, never switching mid-stream):
// - debug.vrscreen.host set: only that host (port from debug.vrscreen.port, default 7777).
// - Otherwise USB first: 127.0.0.1:port, which `adb reverse tcp:7777 tcp:7777` tunnels to the
//   Mac (without a reverse it is refused at once; without a sender adbd closes it before the
//   hello). Meanwhile UDP discovery (port 7778) runs; if USB gives no hello, the client connects
//   to the sender that answered, or else to the last Wi-Fi sender it used (kept in the files dir).
// Then: validates the hello, parses messages, pings for the clock offset, acknowledges frames, and
// drops the connection if nothing at all arrives for 3 s despite the pings (dead Wi-Fi link).
class StreamClient {
public:
    StreamClient(StreamListener& listener, std::string filesDir)
        : listener_(listener), filesDir_(std::move(filesDir)) {}
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
    struct Endpoint {
        enum class Kind { Usb, WiFi, Override };
        Kind kind = Kind::Usb;
        std::string host;
        int port = 0;
        std::string name;  // sender name from ANNOUNCE (Wi-Fi only)

        std::string transport() const;
    };

    void run();
    // Connects and reads the 6-byte hello; returns the socket (hello consumed) or -1.
    int connectWithHello(const Endpoint& endpoint, int connectTimeoutMs, int helloTimeoutMs, std::string* error);
    std::string session(int fd);
    bool handleMessage(uint8_t type, const uint8_t* payload, uint32_t length, std::string* error);
    bool sendPing(int fd);
    bool sendAck(int fd);
    void onPong(const uint8_t* payload, uint32_t length);
    bool sleepInterruptible(int milliseconds);

    // Discovery (UDP, network thread only).
    void openDiscoverySocket();
    void sendDiscover();
    // Collects ANNOUNCE replies until one arrives or `timeoutMs` passes.
    void collectAnnouncements(int timeoutMs, int tcpPortFallback);
    bool loadLastHost(Endpoint* out) const;
    void saveLastHost(const Endpoint& endpoint) const;

    StreamListener& listener_;
    const std::string filesDir_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    std::atomic<int> fd_{-1};  // socket being connected or streamed, for stop()

    int discoveryFd_ = -1;
    std::vector<Endpoint> announced_;  // ANNOUNCE replies to the current round's DISCOVER

    // Per connection (network thread).
    uint32_t framesConsumed_ = 0;
    uint32_t framesAcked_ = 0;
    uint64_t maxCaptureMicros_ = 0;
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
