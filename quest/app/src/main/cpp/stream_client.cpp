#include "stream_client.h"

#include "log.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace qm {
namespace {

constexpr uint8_t kMsgConfig = 0x01;
constexpr uint8_t kMsgFrame = 0x02;
constexpr uint8_t kMsgPing = 0x10;
constexpr uint8_t kMsgPong = 0x11;
constexpr uint8_t kMsgFrameAck = 0x12;
constexpr uint32_t kMaxPayload = 64u << 20;
constexpr size_t kHeaderSize = 5;
constexpr const char* kDefaultHost = "127.0.0.1";
constexpr int kDefaultPort = 7777;
constexpr int64_t kHelloTimeoutMicros = 3'000'000;
constexpr int64_t kStatsIntervalMicros = 1'000'000;
constexpr int64_t kPingIntervalMicros = 500'000;
constexpr int64_t kFastPingIntervalMicros = 100'000;
constexpr int kFastPings = 5;
constexpr size_t kClockWindow = 16;

uint32_t readU32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}

uint64_t readU64(const uint8_t* p) { return (uint64_t{readU32(p)} << 32) | readU32(p + 4); }

void writeU32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

void writeU64(uint8_t* p, uint64_t v) {
    writeU32(p, static_cast<uint32_t>(v >> 32));
    writeU32(p + 4, static_cast<uint32_t>(v));
}

std::string readProperty(const char* name) {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get(name, value);
    return value;
}

std::string errnoString(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

// Non-blocking connect with a 1 s timeout. Returns the connected (non-blocking) socket or -1.
int connectTo(const std::string& host, int port, std::string* error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    const int gai = getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
    if (gai != 0) {
        *error = std::string("resolve: ") + gai_strerror(gai);
        return -1;
    }
    int fd = socket(result->ai_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        *error = errnoString("socket");
        freeaddrinfo(result);
        return -1;
    }
    int rc = connect(fd, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    if (rc != 0 && errno == EINPROGRESS) {
        pollfd p{fd, POLLOUT, 0};
        rc = poll(&p, 1, 1000);
        if (rc == 0) {
            *error = "connect: timed out";
            close(fd);
            return -1;
        }
        int soError = 0;
        socklen_t len = sizeof(soError);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len);
        rc = soError == 0 ? 0 : -1;
        errno = soError;
    }
    if (rc != 0) {
        *error = errnoString("connect");
        close(fd);
        return -1;
    }
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

// Writes all bytes, waiting up to 200 ms for socket space.
bool sendAll(int fd, const uint8_t* data, size_t size) {
    while (size > 0) {
        const ssize_t n = send(fd, data, size, MSG_NOSIGNAL);
        if (n > 0) {
            data += n;
            size -= static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p{fd, POLLOUT, 0};
            if (poll(&p, 1, 200) <= 0) return false;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

}  // namespace

int64_t StreamClient::nowMicros() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000 + ts.tv_nsec / 1000;
}

bool StreamClient::clockOffset(int64_t* offsetMicros) const {
    if (!offsetValid_.load()) return false;
    *offsetMicros = offset_.load();
    return true;
}

void StreamClient::start() {
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread(&StreamClient::run, this);
}

void StreamClient::stop() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        const int fd = fd_.load();
        if (fd >= 0) shutdown(fd, SHUT_RDWR);  // wakes poll() at once
    }
    cv_.notify_all();
    thread_.join();
}

bool StreamClient::sleepInterruptible(int milliseconds) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return stop_.load(); });
    return !stop_.load();
}

void StreamClient::run() {
    pthread_setname_np(pthread_self(), "qm-network");
    const int rc = setpriority(PRIO_PROCESS, gettid(), -10);
    LOGI("Network thread started (nice -10: %s)", rc == 0 ? "ok" : std::strerror(errno));

    std::string lastError;
    while (!stop_) {
        const std::string hostProp = readProperty("debug.questmirror.host");
        const std::string portProp = readProperty("debug.questmirror.port");
        const std::string host = hostProp.empty() ? kDefaultHost : hostProp;
        const int port = portProp.empty() ? kDefaultPort : std::atoi(portProp.c_str());
        const std::string endpoint = host + ":" + std::to_string(port);
        listener_.onConnecting(endpoint, lastError);

        std::string error;
        const int fd = connectTo(host, port, &error);
        if (fd < 0) {
            if (error != lastError) LOGI("Connect to %s failed: %s", endpoint.c_str(), error.c_str());
            lastError = error;
            sleepInterruptible(1000);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fd_ = fd;
            if (stop_) shutdown(fd, SHUT_RDWR);
        }
        const std::string reason = session(fd, endpoint);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fd_ = -1;
        }
        close(fd);
        // adb reverse accepts the TCP connection even when nothing listens on the Mac, so a missing
        // sender shows up as "closed before hello" once per second: log only changes.
        if (reason != lastError) LOGI("Disconnected from %s: %s", endpoint.c_str(), reason.c_str());
        listener_.onDisconnected(reason);
        lastError = reason;
        if (!stop_) sleepInterruptible(1000);
    }
    LOGI("Network thread stopped");
}

std::string StreamClient::session(int fd, const std::string& endpoint) {
    framesConsumed_ = 0;
    framesAcked_ = 0;
    statFrames_ = 0;
    statBytes_ = 0;
    clockSamples_.clear();
    offsetValid_ = false;
    bestRtt_ = -1;

    std::vector<uint8_t> rx(1 << 20);
    size_t head = 0;  // first unparsed byte
    size_t tail = 0;  // end of received data
    bool helloDone = false;
    const int64_t start = nowMicros();
    int64_t nextPing = 0;
    int pingsSent = 0;
    int64_t lastTick = start;

    while (!stop_) {
        const int64_t now = nowMicros();
        if (!helloDone && now - start > kHelloTimeoutMicros) return "no hello from the sender";
        if (helloDone && now >= nextPing) {
            if (!sendPing(fd)) return errnoString("send PING");
            ++pingsSent;
            nextPing = now + (pingsSent < kFastPings ? kFastPingIntervalMicros : kPingIntervalMicros);
        }
        if (helloDone && now - lastTick >= kStatsIntervalMicros) {
            LinkStats stats;
            stats.seconds = static_cast<double>(now - lastTick) / 1e6;
            stats.frames = statFrames_;
            stats.bytes = statBytes_;
            stats.rttMicros = bestRtt_.load();
            statFrames_ = statBytes_ = 0;
            lastTick = now;
            listener_.onStatsTick(stats);
        }

        int64_t waitMicros = 100'000;
        if (helloDone) waitMicros = std::min(waitMicros, std::max<int64_t>(nextPing - now, 0));
        pollfd p{fd, POLLIN, 0};
        const int ready = poll(&p, 1, static_cast<int>((waitMicros + 999) / 1000));
        if (ready < 0 && errno != EINTR) return errnoString("poll");
        if (ready <= 0) continue;

        if (tail == rx.size()) {
            if (head > 0) {
                std::memmove(rx.data(), rx.data() + head, tail - head);
                tail -= head;
                head = 0;
            } else {
                rx.resize(rx.size() * 2);
            }
        }
        const ssize_t n = recv(fd, rx.data() + tail, rx.size() - tail, 0);
        if (n == 0) {
            if (stop_) return "stopped";
            return helloDone ? "connection closed by the sender" : "no sender (closed before hello)";
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            return errnoString("recv");
        }
        tail += static_cast<size_t>(n);
        statBytes_ += static_cast<uint64_t>(n);

        if (!helloDone) {
            if (tail - head < 6) continue;
            const uint8_t* h = rx.data() + head;
            if (std::memcmp(h, "QMIR", 4) != 0) return "bad hello magic";
            const uint16_t version = static_cast<uint16_t>((h[4] << 8) | h[5]);
            if (version != 1) return "unsupported protocol version " + std::to_string(version);
            head += 6;
            helloDone = true;
            LOGI("Connected to %s (hello OK, protocol v1)", endpoint.c_str());
            listener_.onConnected(endpoint);
        }

        // Parse every complete message; acknowledge consumed frames once per batch.
        while (tail - head >= kHeaderSize) {
            const uint8_t* m = rx.data() + head;
            const uint8_t type = m[0];
            const uint32_t length = readU32(m + 1);
            if (length > kMaxPayload) return "message too large (" + std::to_string(length) + " bytes)";
            if (tail - head < kHeaderSize + length) {
                // Make room for the whole message.
                if (kHeaderSize + length > rx.size() - head) {
                    std::memmove(rx.data(), rx.data() + head, tail - head);
                    tail -= head;
                    head = 0;
                    if (kHeaderSize + length > rx.size()) rx.resize(kHeaderSize + length);
                }
                break;
            }
            std::string error;
            if (!handleMessage(type, m + kHeaderSize, length, &error)) return error;
            head += kHeaderSize + length;
            if (stop_) return "stopped";
        }
        if (head == tail) head = tail = 0;
        if (framesAcked_ != framesConsumed_ && !sendAck(fd)) return errnoString("send FRAME_ACK");
    }
    return "stopped";
}

bool StreamClient::handleMessage(uint8_t type, const uint8_t* payload, uint32_t length, std::string* error) {
    switch (type) {
        case kMsgConfig: {
            if (length < 9) {
                *error = "short CONFIG";
                return false;
            }
            StreamConfig config;
            if (payload[0] != static_cast<uint8_t>(Codec::H264) && payload[0] != static_cast<uint8_t>(Codec::HEVC)) {
                *error = "unknown codec " + std::to_string(payload[0]);
                return false;
            }
            config.codec = static_cast<Codec>(payload[0]);
            config.width = static_cast<int32_t>(readU32(payload + 1));
            config.height = static_cast<int32_t>(readU32(payload + 5));
            config.parameterSets.assign(payload + 9, payload + length);
            config.payload.assign(payload, payload + length);
            LOGI("CONFIG: %s %dx%d, %zu bytes of parameter sets", config.codec == Codec::HEVC ? "HEVC" : "H.264",
                 config.width, config.height, config.parameterSets.size());
            if (!listener_.onConfig(config)) {
                *error = "stream setup failed";
                return false;
            }
            return true;
        }
        case kMsgFrame: {
            if (length < 9) {
                *error = "short FRAME";
                return false;
            }
            EncodedFrame frame;
            frame.keyframe = (payload[0] & 0x01) != 0;
            frame.captureTimeMicros = readU64(payload + 1);
            frame.data = payload + 9;
            frame.size = length - 9;
            ++statFrames_;
            const bool ok = listener_.onFrame(frame);
            ++framesConsumed_;  // handed to the decoder or deliberately dropped: either way consumed
            if (!ok) {
                *error = "decoder error";
                return false;
            }
            return true;
        }
        case kMsgPong:
            onPong(payload, length);
            return true;
        default:
            return true;  // unknown types are skipped
    }
}

bool StreamClient::sendPing(int fd) {
    uint8_t msg[kHeaderSize + 8];
    msg[0] = kMsgPing;
    writeU32(msg + 1, 8);
    writeU64(msg + kHeaderSize, static_cast<uint64_t>(nowMicros()));
    return sendAll(fd, msg, sizeof(msg));
}

bool StreamClient::sendAck(int fd) {
    uint8_t msg[kHeaderSize + 4];
    msg[0] = kMsgFrameAck;
    writeU32(msg + 1, 4);
    writeU32(msg + kHeaderSize, framesConsumed_);
    if (!sendAll(fd, msg, sizeof(msg))) return false;
    framesAcked_ = framesConsumed_;
    return true;
}

void StreamClient::onPong(const uint8_t* payload, uint32_t length) {
    if (length < 16) return;
    const int64_t t1 = nowMicros();
    const int64_t t0 = static_cast<int64_t>(readU64(payload));
    const int64_t ts = static_cast<int64_t>(readU64(payload + 8));
    const int64_t rtt = t1 - t0;
    if (rtt < 0) return;
    clockSamples_.push_back({rtt, ts - (t0 + rtt / 2)});
    if (clockSamples_.size() > kClockWindow) clockSamples_.pop_front();
    const auto best = std::min_element(clockSamples_.begin(), clockSamples_.end(),
                                       [](const ClockSample& a, const ClockSample& b) { return a.rtt < b.rtt; });
    const bool first = !offsetValid_.load();
    offset_ = best->offset;
    bestRtt_ = best->rtt;
    offsetValid_ = true;
    if (first) LOGI("Clock offset %" PRId64 " us (rtt %" PRId64 " us)", best->offset, best->rtt);
}

}  // namespace qm
