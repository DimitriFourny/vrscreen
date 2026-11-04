#include "stream_client.h"

#include "log.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace qm {
namespace {

constexpr uint8_t kMsgConfig = 0x01;
constexpr uint8_t kMsgFrame = 0x02;
constexpr uint8_t kMsgControl = 0x03;
constexpr uint8_t kMsgPing = 0x10;
constexpr uint8_t kMsgPong = 0x11;
constexpr uint8_t kMsgFrameAck = 0x12;
constexpr uint32_t kMaxPayload = 64u << 20;
constexpr size_t kHeaderSize = 5;
constexpr int kDefaultPort = 7777;
constexpr uint16_t kDiscoveryPort = 7778;
constexpr uint8_t kDiscover = 0x01;
constexpr uint8_t kAnnounce = 0x02;
constexpr int64_t kStatsIntervalMicros = 1'000'000;
constexpr int64_t kPingIntervalMicros = 500'000;
constexpr int64_t kFastPingIntervalMicros = 100'000;
constexpr int64_t kDeadLinkMicros = 3'000'000;  // nothing received despite pings every 500 ms
constexpr int kFastPings = 5;
constexpr size_t kClockWindow = 16;
constexpr int kRoundMillis = 1000;
constexpr int kAnnounceWaitMillis = 500;

uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

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

std::string StreamClient::Endpoint::transport() const {
    switch (kind) {
        case Kind::Usb: return "USB (adb reverse)";
        case Kind::WiFi: return "Wi-Fi " + host + (name.empty() ? "" : " (" + name + ")");
        case Kind::Override: return host + ":" + std::to_string(port) + " (debug.questmirror.host)";
    }
    return host;
}

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
    if (milliseconds <= 0) return !stop_.load();
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return stop_.load(); });
    return !stop_.load();
}

void StreamClient::run() {
    pthread_setname_np(pthread_self(), "qm-network");
    const int rc = setpriority(PRIO_PROCESS, gettid(), -10);
    LOGI("Network thread started (nice -10: %s)", rc == 0 ? "ok" : std::strerror(errno));
    openDiscoverySocket();

    SearchState search;
    std::string lastLogged;
    while (!stop_) {
        const int64_t roundStart = nowMicros();
        const std::string hostProp = readProperty("debug.questmirror.host");
        const std::string portProp = readProperty("debug.questmirror.port");
        const int port = portProp.empty() ? kDefaultPort : std::atoi(portProp.c_str());

        Endpoint chosen;
        int fd = -1;
        if (!hostProp.empty()) {
            chosen = {Endpoint::Kind::Override, hostProp, port, ""};
            search.overrideEndpoint = hostProp + ":" + std::to_string(port);
            listener_.onSearching(search);
            std::string error;
            fd = connectWithHello(chosen, 1000, 3000, &error);
            search.wifiResult = error;
        } else {
            search.overrideEndpoint.clear();
            listener_.onSearching(search);
            announced_.clear();
            sendDiscover();

            // USB first: preferred whenever the tunnel and a sender are there.
            chosen = {Endpoint::Kind::Usb, "127.0.0.1", port, ""};
            std::string error;
            fd = connectWithHello(chosen, 300, 1000, &error);
            if (fd < 0) {
                if (error.find("refused") != std::string::npos) {
                    search.usbResult = "no adb reverse tcp:" + std::to_string(port);
                } else if (error.find("closed before hello") != std::string::npos) {
                    search.usbResult = "adb reverse ok, no sender";
                } else {
                    search.usbResult = error;
                }
            }

            // Then Wi-Fi: the sender(s) that answered discovery, else the last one used.
            if (fd < 0 && !stop_) {
                collectAnnouncements(kAnnounceWaitMillis, port);
                std::vector<Endpoint> candidates = announced_;
                Endpoint last;
                if (candidates.empty() && loadLastHost(&last)) candidates.push_back(last);
                search.wifiResult = candidates.empty() ? "no reply to discovery" : "";
                for (const Endpoint& candidate : candidates) {
                    fd = connectWithHello(candidate, 1000, 2000, &error);
                    if (fd >= 0) {
                        chosen = candidate;
                        saveLastHost(candidate);
                        break;
                    }
                    search.wifiResult = candidate.host + ": " + error;
                }
            }
        }

        if (fd < 0) {
            const std::string summary = search.overrideEndpoint.empty()
                                            ? "USB: " + search.usbResult + " | Wi-Fi: " + search.wifiResult
                                            : search.overrideEndpoint + ": " + search.wifiResult;
            if (summary != lastLogged) LOGI("No sender yet (%s)", summary.c_str());
            lastLogged = summary;
            listener_.onSearching(search);
            sleepInterruptible(kRoundMillis - static_cast<int>((nowMicros() - roundStart) / 1000));
            continue;
        }

        lastLogged.clear();
        LOGI("Connected over %s, %s:%d (hello OK, protocol v1)", chosen.transport().c_str(), chosen.host.c_str(),
             chosen.port);
        listener_.onConnected(chosen.transport());
        const std::string reason = session(fd);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fd_ = -1;
        }
        close(fd);
        LOGI("Disconnected from %s: %s", chosen.transport().c_str(), reason.c_str());
        listener_.onDisconnected(reason);
        search = SearchState{};
        search.lastDisconnect = chosen.transport() + ": " + reason;
        if (!stop_) sleepInterruptible(300);
    }
    if (discoveryFd_ >= 0) close(discoveryFd_);
    discoveryFd_ = -1;
    LOGI("Network thread stopped");
}

int StreamClient::connectWithHello(const Endpoint& endpoint, int connectTimeoutMs, int helloTimeoutMs,
                                   std::string* error) {
    // Numeric hosts only (discovery and the override prop give IPs): a DNS lookup could not be
    // interrupted by stop(), which runs on the XR thread.
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(endpoint.port);
    const int gai = getaddrinfo(endpoint.host.c_str(), service.c_str(), &hints, &result);
    if (gai != 0) {
        *error = endpoint.host + " is not an IP address";
        return -1;
    }

    auto publish = [this](int fd) {
        std::lock_guard<std::mutex> lock(mutex_);
        fd_ = fd;
        if (fd >= 0 && stop_) shutdown(fd, SHUT_RDWR);
    };
    auto closeFd = [&](int fd) {
        publish(-1);
        close(fd);
    };

    // Try every address; poll in 50 ms slices so stop() is noticed promptly.
    int fd = -1;
    *error = "no address";
    for (addrinfo* ai = result; ai != nullptr && fd < 0 && !stop_; ai = ai->ai_next) {
        const int s = socket(ai->ai_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (s < 0) {
            *error = errnoString("socket");
            continue;
        }
        publish(s);
        if (connect(s, ai->ai_addr, ai->ai_addrlen) == 0) {
            fd = s;
            break;
        }
        if (errno != EINPROGRESS) {
            *error = errnoString("connect");
            closeFd(s);
            continue;
        }
        const int64_t deadline = nowMicros() + int64_t{connectTimeoutMs} * 1000;
        bool connected = false;
        *error = "connect: timed out";
        while (!stop_ && nowMicros() < deadline) {
            pollfd p{s, POLLOUT, 0};
            if (poll(&p, 1, 50) <= 0) continue;
            int soError = 0;
            socklen_t len = sizeof(soError);
            getsockopt(s, SOL_SOCKET, SO_ERROR, &soError, &len);
            errno = soError;
            if (soError == 0) connected = true;
            else *error = errnoString("connect");
            break;
        }
        if (connected) {
            fd = s;
        } else {
            closeFd(s);
        }
    }
    freeaddrinfo(result);
    if (stop_) {
        if (fd >= 0) closeFd(fd);
        *error = "stopped";
        return -1;
    }
    if (fd < 0) return -1;
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    auto fail = [&](const std::string& message) {
        *error = message;
        closeFd(fd);
        return -1;
    };
    // The hello: read exactly 6 bytes so everything after it stays for session().
    uint8_t hello[6];
    size_t got = 0;
    const int64_t deadline = nowMicros() + int64_t{helloTimeoutMs} * 1000;
    while (got < sizeof(hello)) {
        if (stop_) return fail("stopped");
        const int64_t left = deadline - nowMicros();
        if (left <= 0) return fail("no hello from the sender");
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, static_cast<int>(std::min<int64_t>(left / 1000 + 1, 50))) <= 0) continue;
        const ssize_t n = recv(fd, hello + got, sizeof(hello) - got, 0);
        if (n == 0) return fail("closed before hello");
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            return fail(errnoString("recv"));
        }
        got += static_cast<size_t>(n);
    }
    if (std::memcmp(hello, "QMIR", 4) != 0) return fail("bad hello magic");
    if (readU16(hello + 4) != 1) return fail("unsupported protocol version " + std::to_string(readU16(hello + 4)));
    return fd;
}

void StreamClient::openDiscoverySocket() {
    discoveryFd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (discoveryFd_ < 0) {
        LOGE("Discovery disabled: %s", std::strerror(errno));
        return;
    }
    const int one = 1;
    setsockopt(discoveryFd_, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    any.sin_port = 0;
    if (bind(discoveryFd_, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0) {
        LOGE("Discovery disabled: bind: %s", std::strerror(errno));
        close(discoveryFd_);
        discoveryFd_ = -1;
    }
}

void StreamClient::sendDiscover() {
    if (discoveryFd_ < 0) return;
    const uint8_t message[7] = {'Q', 'M', 'I', 'R', kDiscover, 0, 1};
    // The limited broadcast plus each interface's directed broadcast (e.g. 192.168.1.255 on wlan0).
    std::vector<in_addr_t> targets = {htonl(INADDR_BROADCAST)};
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        for (ifaddrs* i = interfaces; i != nullptr; i = i->ifa_next) {
            if (i->ifa_addr == nullptr || i->ifa_addr->sa_family != AF_INET) continue;
            if ((i->ifa_flags & IFF_BROADCAST) == 0 || (i->ifa_flags & IFF_LOOPBACK) != 0) continue;
            if ((i->ifa_flags & IFF_UP) == 0 || i->ifa_broadaddr == nullptr) continue;
            const in_addr_t b = reinterpret_cast<sockaddr_in*>(i->ifa_broadaddr)->sin_addr.s_addr;
            if (std::find(targets.begin(), targets.end(), b) == targets.end()) targets.push_back(b);
        }
        freeifaddrs(interfaces);
    }
    for (const in_addr_t target : targets) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(kDiscoveryPort);
        to.sin_addr.s_addr = target;
        sendto(discoveryFd_, message, sizeof(message), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }
}

void StreamClient::collectAnnouncements(int timeoutMs, int tcpPortFallback) {
    if (discoveryFd_ < 0) return;
    const int64_t deadline = nowMicros() + int64_t{timeoutMs} * 1000;
    while (!stop_) {
        uint8_t buffer[512];
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        const ssize_t n =
            recvfrom(discoveryFd_, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n >= 10 && std::memcmp(buffer, "QMIR", 4) == 0 && buffer[4] == kAnnounce && readU16(buffer + 5) == 1) {
            Endpoint e;
            e.kind = Endpoint::Kind::WiFi;
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            e.host = ip;
            e.port = readU16(buffer + 7);
            if (e.port == 0) e.port = tcpPortFallback;
            const size_t nameLen = std::min<size_t>(buffer[9], static_cast<size_t>(n) - 10);
            e.name.assign(reinterpret_cast<const char*>(buffer + 10), nameLen);
            const bool known = std::any_of(announced_.begin(), announced_.end(), [&](const Endpoint& a) {
                return a.host == e.host && a.port == e.port;
            });
            if (!known) announced_.push_back(e);
            continue;  // drain everything already queued
        }
        if (n >= 0) continue;  // malformed or not an ANNOUNCE: ignore
        if (!announced_.empty()) return;
        const int64_t left = deadline - nowMicros();
        if (left <= 0) return;
        pollfd p{discoveryFd_, POLLIN, 0};
        poll(&p, 1, static_cast<int>(std::min<int64_t>(left / 1000 + 1, 50)));
    }
}

bool StreamClient::loadLastHost(Endpoint* out) const {
    if (filesDir_.empty()) return false;
    FILE* f = std::fopen((filesDir_ + "/last_host.txt").c_str(), "r");
    if (f == nullptr) return false;
    char host[64] = {};
    int port = 0;
    char name[128] = {};
    const int fields = std::fscanf(f, "%63s %d %127[^\n]", host, &port, name);
    std::fclose(f);
    if (fields < 2 || port <= 0) return false;
    *out = {Endpoint::Kind::WiFi, host, port, fields == 3 ? name : ""};
    return true;
}

void StreamClient::saveLastHost(const Endpoint& endpoint) const {
    if (filesDir_.empty()) return;
    FILE* f = std::fopen((filesDir_ + "/last_host.txt").c_str(), "w");
    if (f == nullptr) return;
    std::fprintf(f, "%s %d %s\n", endpoint.host.c_str(), endpoint.port, endpoint.name.c_str());
    std::fclose(f);
}

std::string StreamClient::session(int fd) {
    framesConsumed_ = 0;
    framesAcked_ = 0;
    maxCaptureMicros_ = 0;
    statFrames_ = 0;
    statBytes_ = 0;
    clockSamples_.clear();
    offsetValid_ = false;
    bestRtt_ = -1;

    std::vector<uint8_t> rx(1 << 20);
    size_t head = 0;  // first unparsed byte
    size_t tail = 0;  // end of received data
    const int64_t start = nowMicros();
    int64_t nextPing = start;
    int pingsSent = 0;
    int64_t lastTick = start;
    int64_t lastReceive = start;

    while (!stop_) {
        const int64_t now = nowMicros();
        if (now >= nextPing) {
            if (!sendPing(fd)) return errnoString("send PING");
            ++pingsSent;
            nextPing = now + (pingsSent < kFastPings ? kFastPingIntervalMicros : kPingIntervalMicros);
        }
        if (now - lastTick >= kStatsIntervalMicros) {
            LinkStats stats;
            stats.seconds = static_cast<double>(now - lastTick) / 1e6;
            stats.frames = statFrames_;
            stats.bytes = statBytes_;
            stats.rttMicros = bestRtt_.load();
            statFrames_ = statBytes_ = 0;
            lastTick = now;
            if (!listener_.onStatsTick(stats)) return "decoder failed";
        }

        const int64_t waitMicros = std::min<int64_t>(100'000, std::max<int64_t>(nextPing - now, 0));
        pollfd p{fd, POLLIN, 0};
        const int ready = poll(&p, 1, static_cast<int>((waitMicros + 999) / 1000));
        if (ready < 0 && errno != EINTR) return errnoString("poll");
        if (ready <= 0) {
            // Idle: the sender answers every PING, so silence this long means the link is gone.
            if (nowMicros() - lastReceive > kDeadLinkMicros) return "nothing received for 3 s (link lost?)";
            continue;
        }

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
        if (n == 0) return stop_ ? "stopped" : "connection closed by the sender";
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            return errnoString("recv");
        }
        lastReceive = nowMicros();
        tail += static_cast<size_t>(n);
        statBytes_ += static_cast<uint64_t>(n);

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
            {
                const uint32_t w = readU32(payload + 1), h = readU32(payload + 5);
                if (w == 0 || h == 0 || w > 8192 || h > 8192 || (w & 1) || (h & 1)) {
                    *error = "invalid CONFIG size " + std::to_string(w) + "x" + std::to_string(h);
                    return false;
                }
            }
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
            frame.freshCapture = frame.captureTimeMicros > maxCaptureMicros_;
            maxCaptureMicros_ = std::max(maxCaptureMicros_, frame.captureTimeMicros);
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
        case kMsgControl: {
            // command u8 | args; unknown commands and extra bytes are ignored.
            if (length < 1) return true;
            PlacementCommand command;
            uint32_t needed = 1;
            switch (payload[0]) {
                case 0x01: command.kind = PlacementCommand::Kind::Recenter; break;
                case 0x02: command.kind = PlacementCommand::Kind::Move; needed = 3; break;
                case 0x03: command.kind = PlacementCommand::Kind::Distance; needed = 2; break;
                case 0x04: command.kind = PlacementCommand::Kind::Size; needed = 2; break;
                case 0x05: command.kind = PlacementCommand::Kind::Reset; break;
                default:
                    LOGI("CONTROL: unknown command 0x%02x ignored", payload[0]);
                    return true;
            }
            if (length < needed) {
                LOGW("CONTROL 0x%02x: %u byte(s), needs %u; ignored", payload[0], length, needed);
                return true;
            }
            if (needed >= 2) command.a = static_cast<int8_t>(payload[1]);
            if (needed >= 3) command.b = static_cast<int8_t>(payload[2]);
            listener_.onControl(command);
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
