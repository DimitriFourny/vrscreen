#include "server.h"

#include <ws2tcpip.h>

#include <algorithm>

#include "log.h"
#include "util.h"

namespace vr {
namespace {
constexpr size_t kMaxSendSlice = 64 * 1024;

std::string describePeer(const sockaddr_storage& addr) {
    char host[NI_MAXHOST] = {}, service[NI_MAXSERV] = {};
    if (getnameinfo(reinterpret_cast<const sockaddr*>(&addr), sizeof addr, host, sizeof host, service, sizeof service,
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0)
        return "unknown";
    std::string h = host;
    if (startsWith(h, "::ffff:")) h = h.substr(7);
    return h + ":" + service;
}
}  // namespace

size_t ClientSession::unsentBytes() const {
    size_t n = 0;
    for (const auto& p : pending) n += p.data.size() - p.written;
    return n;
}

MirrorServer::MirrorServer(EventLoop& loop, uint16_t port, uint32_t maxInFlight)
    : loop_(loop), port_(port), maxInFlight_(maxInFlight), headsetCommands_(Settings{}.commands()) {}

MirrorServer::~MirrorServer() { stop(); }

bool MirrorServer::start(int& error) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        error = WSAGetLastError();
        return false;
    }
    // No SO_REUSEADDR: on Windows it would let a second sender steal the port.
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(s, 8) != 0) {
        error = WSAGetLastError();
        closesocket(s);
        return false;
    }
    listen_ = s;
    listenEvent_ = WSACreateEvent();
    WSAEventSelect(listen_, listenEvent_, FD_ACCEPT);
    loop_.addHandle(listenEvent_, [this] { onListenEvent(); });
    logf("server: listening on 0.0.0.0:%u", port_);
    return true;
}

void MirrorServer::stop() {
    if (active_) drop(active_, "server shutting down");
    if (listen_ != INVALID_SOCKET) {
        loop_.removeHandle(listenEvent_);
        closesocket(listen_);
        WSACloseEvent(listenEvent_);
        listen_ = INVALID_SOCKET;
        listenEvent_ = WSA_INVALID_EVENT;
    }
}

void MirrorServer::onListenEvent() {
    WSANETWORKEVENTS ne{};
    WSAEnumNetworkEvents(listen_, listenEvent_, &ne);
    for (;;) {
        sockaddr_storage addr{};
        int len = sizeof addr;
        SOCKET s = accept(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
        if (s == INVALID_SOCKET) {
            const int e = WSAGetLastError();
            if (e != WSAEWOULDBLOCK) log("server: accept failed: " + win32ErrorText(DWORD(e)));
            return;
        }
        BOOL one = TRUE;
        int sendBuffer = kSendBuffer;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBuffer), sizeof sendBuffer);

        auto client = std::make_shared<ClientSession>(maxInFlight_);
        client->socket = s;
        client->address = describePeer(addr);
        client->event = WSACreateEvent();
        WSAEventSelect(s, client->event, FD_READ | FD_WRITE | FD_CLOSE);  // also makes it non-blocking
        if (active_) drop(active_, "replaced by " + client->address);
        active_ = client;
        log("server: client " + client->address + " connected");
        if (onActiveClientChanged) onActiveClientChanged(&client->address);
        loop_.addHandle(client->event, [this, client] { onClientEvent(client); });

        send(client, proto::hello());
        if (cachedConfig) {
            send(client, cachedConfig->encoded());
            client->sentConfig = cachedConfig;
        }
        // Absolute state the sender owns; clients apply it whenever it arrives.
        for (const auto& c : headsetCommands_) send(client, c.encoded());
        if (audioConfig) {
            send(client, audioConfig->encoded());
            client->sentAudioConfig = audioConfig;
        }
        if (!controlOnConnect.empty()) sendControlOnConnect(client);
        if (onClientReady) onClientReady(client);
    }
}

void MirrorServer::sendControlOnConnect(const std::shared_ptr<ClientSession>& client) {
    long long offsetMicros = 0;
    size_t count = 0;
    std::vector<std::pair<long long, proto::ControlCommand>> schedule;
    for (const auto& step : controlOnConnect) {
        if (step.wait) {
            offsetMicros += step.waitMs * 1000LL;
        } else {
            schedule.emplace_back(offsetMicros, step.command);
            offsetMicros += 16'667;
        }
    }
    logf("server: sending %zu CONTROL command(s) to %s over %.2f s (--debug-control-on-connect)", schedule.size(),
         client->address.c_str(), offsetMicros / 1e6);
    for (const auto& [offset, command] : schedule) {
        const size_t index = ++count;
        const size_t total = schedule.size();
        std::weak_ptr<ClientSession> weak = client;
        loop_.postDelayed(std::chrono::milliseconds(offset / 1000), [this, weak, command, index, total] {
            auto c = weak.lock();
            if (!c || !c->active) return;
            send(c, command.encoded());
            if (total <= 20 || index == total)
                logf("server: sent CONTROL %s to %s (%zu/%zu)", command.describe().c_str(), c->address.c_str(), index, total);
        });
    }
}

void MirrorServer::drop(const std::shared_ptr<ClientSession>& client, const std::string& reason) {
    if (!client->active) return;
    client->active = false;
    log("server: client " + client->address + " disconnected (" + reason + ")");
    client->pending.clear();
    loop_.removeHandle(client->event);
    closesocket(client->socket);
    WSACloseEvent(client->event);
    client->socket = INVALID_SOCKET;
    client->event = WSA_INVALID_EVENT;
    if (active_ == client) {
        active_.reset();
        if (onActiveClientChanged) onActiveClientChanged(nullptr);
    }
}

void MirrorServer::onClientEvent(const std::shared_ptr<ClientSession>& client) {
    if (!client->active) return;
    WSANETWORKEVENTS ne{};
    if (WSAEnumNetworkEvents(client->socket, client->event, &ne) != 0) {
        drop(client, "WSAEnumNetworkEvents: " + win32ErrorText(DWORD(WSAGetLastError())));
        return;
    }
    if (ne.lNetworkEvents & FD_READ) readAvailable(client);
    if (client->active && (ne.lNetworkEvents & FD_WRITE)) flush(client);
    if (client->active && (ne.lNetworkEvents & FD_CLOSE)) {
        readAvailable(client);
        const int e = ne.iErrorCode[FD_CLOSE_BIT];
        drop(client, e ? "connection lost: " + win32ErrorText(DWORD(e)) : "closed by peer");
    }
}

bool MirrorServer::send(const std::shared_ptr<ClientSession>& client, proto::Bytes data, std::function<void()> completion,
                        bool isFrame) {
    if (!client || !client->active) return false;
    if (isFrame) client->ackWindow.recordSent();
    ClientSession::Pending entry;
    entry.data = std::move(data);
    entry.completion = std::move(completion);
    entry.isFrame = isFrame;
    enqueue(client, std::move(entry));
    return true;
}

void MirrorServer::sendAudio(proto::AudioMessage chunk) {
    if (!audioConfig || !active_ || !active_->active) return;
    auto client = active_;
    if (client->sentAudioConfig != audioConfig) {
        send(client, audioConfig->encoded());
        client->sentAudioConfig = audioConfig;
    }
    const uint64_t now = nowMicros();
    dropStaleAudio(*client, now);
    if (now > chunk.captureMicros && now - chunk.captureMicros > kMaxAudioAgeMicros) {
        ++audioDropped_;
        return;
    }
    ++audioQueued_;
    ClientSession::Pending entry;
    entry.audioMicros = chunk.captureMicros;
    entry.data = chunk.encoded();
    enqueue(client, std::move(entry));
}

std::optional<std::string> MirrorServer::sendControl(const proto::ControlCommand& command) {
    if (!active_ || !send(active_, command.encoded())) return std::nullopt;
    return active_->address;
}

void MirrorServer::updateHeadsetSettings(const Settings& settings) {
    const auto old = headsetCommands_;
    headsetCommands_ = settings.commands();
    if (!active_) return;
    for (size_t i = 0; i < headsetCommands_.size(); ++i) {
        if (i >= old.size() || !(headsetCommands_[i] == old[i])) send(active_, headsetCommands_[i].encoded());
    }
}

void MirrorServer::enqueue(const std::shared_ptr<ClientSession>& client, ClientSession::Pending entry) {
    bytesSent_ += long long(entry.data.size());
    auto& q = client->pending;
    auto pos = q.end();
    if (entry.audioMicros) {
        // Overtake trailing FRAMEs that have not started; anything else keeps its place.
        while (pos != q.begin() && std::prev(pos)->isFrame && std::prev(pos)->written == 0) --pos;
    }
    q.insert(pos, std::move(entry));
    flush(client);
}

void MirrorServer::dropStaleAudio(ClientSession& client, uint64_t now) {
    auto& q = client.pending;
    for (auto it = q.begin(); it != q.end();) {
        if (it->written == 0 && it->audioMicros && now > *it->audioMicros && now - *it->audioMicros > kMaxAudioAgeMicros) {
            bytesSent_ -= long long(it->data.size());
            ++audioDropped_;
            it = q.erase(it);
        } else {
            ++it;
        }
    }
}

void MirrorServer::flush(const std::shared_ptr<ClientSession>& client) {
    if (!client->active) return;
    bool hasUnstartedAudio = false;
    for (const auto& p : client->pending) hasUnstartedAudio |= p.audioMicros && p.written == 0;
    if (hasUnstartedAudio) dropStaleAudio(*client, nowMicros());
    while (!client->pending.empty()) {
        auto& first = client->pending.front();
        const size_t remaining = first.data.size() - first.written;
        const int slice = int(std::min(remaining, kMaxSendSlice));
        const int n = ::send(client->socket, reinterpret_cast<const char*>(first.data.data() + first.written), slice, 0);
        if (n == SOCKET_ERROR) {
            const int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) return;  // FD_WRITE resumes
            drop(client, "write error: " + win32ErrorText(DWORD(e)));
            return;
        }
        first.written += size_t(n);
        if (first.written == first.data.size()) {
            auto completion = std::move(first.completion);
            client->pending.pop_front();
            if (completion) loop_.post(std::move(completion));
        }
    }
}

void MirrorServer::readAvailable(const std::shared_ptr<ClientSession>& client) {
    uint8_t buffer[16 * 1024];
    for (;;) {
        if (!client->active) return;
        const int n = recv(client->socket, reinterpret_cast<char*>(buffer), sizeof buffer, 0);
        if (n == 0) return;  // FD_CLOSE follows
        if (n == SOCKET_ERROR) {
            const int e = WSAGetLastError();
            if (e != WSAEWOULDBLOCK) drop(client, "read error: " + win32ErrorText(DWORD(e)));
            return;
        }
        client->reader.append(buffer, size_t(n));
        uint8_t type;
        proto::Bytes payload;
        while (client->active && client->reader.next(type, payload)) handle(client, type, payload);
        if (!client->reader.error().empty()) {
            drop(client, "protocol error: " + client->reader.error());
            return;
        }
    }
}

void MirrorServer::handle(const std::shared_ptr<ClientSession>& client, uint8_t type, const proto::Bytes& payload) {
    switch (type) {
        case proto::kPing:
            if (payload.size() < 8) return drop(client, "protocol error: PING payload shorter than 8 bytes");
            send(client, proto::pongMessage(getU64(payload.data()), nowMicros()));
            break;
        case proto::kFrameAck: {
            if (payload.size() < 4) return drop(client, "protocol error: FRAME_ACK payload shorter than 4 bytes");
            const uint32_t consumed = getU32(payload.data());
            const bool wasActive = client->ackWindow.active();
            if (!client->ackWindow.recordAck(consumed)) {
                logf("server: client %s sent invalid FRAME_ACK %u (acked %u, sent %u); ignored", client->address.c_str(),
                     consumed, client->ackWindow.framesAcked(), client->ackWindow.framesSent());
                return;
            }
            if (!wasActive) {
                int size = kAckedSendBuffer;
                setsockopt(client->socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof size);
                logf("server: client %s acknowledges frames; at most %u in flight, send buffer %d KiB",
                     client->address.c_str(), maxInFlight_, size / 1024);
            }
            if (onFrameAck) onFrameAck(client);
            break;
        }
        case proto::kHeadsetStatus:
            if (payload.size() < 2) {
                log("server: client " + client->address + " sent a malformed HEADSET_STATUS (payload shorter than 2 bytes); ignored");
                return;
            }
            if (client == active_ && onHeadsetStatus) onHeadsetStatus({payload[0], payload[1]});
            break;
        default:
            break;  // unknown types and server-to-client types are skipped
    }
}

}  // namespace vr
