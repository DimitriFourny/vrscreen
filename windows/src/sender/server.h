// TCP server (port of the Mac MirrorServer): one active client, a new connection replaces the
// previous one. Everything runs on the event loop thread.
//
// The kernel send buffer is fixed (SO_SNDBUF 128 KiB, 512 KiB once the client acknowledges
// frames) and every message waits in `pending` until written, so "frame fully written" is a
// meaningful backpressure signal. Sends are issued in slices of at most 64 KiB: a non-blocking
// Winsock send otherwise accepts a whole keyframe at once, whatever SO_SNDBUF says.
#pragma once

#include <winsock2.h>

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "event_loop.h"
#include "options.h"
#include "protocol.h"
#include "settings.h"

namespace vr {

class ClientSession {
public:
    std::string address;
    // FRAMEs queued and not fully written yet (backpressure for clients without FRAME_ACK).
    int framesUnsent = 0;
    proto::FrameAckWindow ackWindow;
    bool needsKeyframe = true;
    std::optional<proto::ConfigMessage> sentConfig;
    std::optional<proto::AudioConfigMessage> sentAudioConfig;
    bool active = true;
    size_t unsentBytes() const;

    explicit ClientSession(uint32_t maxInFlight) : ackWindow(maxInFlight) {}

private:
    friend class MirrorServer;
    struct Pending {
        proto::Bytes data;
        size_t written = 0;
        std::function<void()> completion;
        std::optional<uint64_t> audioMicros;
        bool isFrame = false;
    };
    SOCKET socket = INVALID_SOCKET;
    WSAEVENT event = WSA_INVALID_EVENT;
    proto::MessageReader reader{false};
    std::deque<Pending> pending;
};

class MirrorServer {
public:
    static constexpr int kSendBuffer = 128 * 1024;
    static constexpr int kAckedSendBuffer = 512 * 1024;
    static constexpr uint64_t kMaxAudioAgeMicros = 150'000;

    MirrorServer(EventLoop& loop, uint16_t port, uint32_t maxInFlight);
    ~MirrorServer();

    // Returns false with the WSA error in `error` (WSAEADDRINUSE: another process has the port).
    bool start(int& error);
    void stop();
    bool listening() const { return listen_ != INVALID_SOCKET; }
    uint16_t port() const { return port_; }

    // Called right after a client connected and got hello, CONFIG and settings.
    std::function<void(const std::shared_ptr<ClientSession>&)> onClientReady;
    std::function<void(const std::shared_ptr<ClientSession>&)> onFrameAck;
    std::function<void(const std::string* address)> onActiveClientChanged;
    std::function<void(const proto::HeadsetStatus&)> onHeadsetStatus;

    std::optional<proto::ConfigMessage> cachedConfig;
    std::vector<DebugControlStep> controlOnConnect;
    // Format of the AUDIO produced; nullopt while audio is off.
    std::optional<proto::AudioConfigMessage> audioConfig;

    const std::shared_ptr<ClientSession>& activeClient() const { return active_; }
    long long bytesSent() const { return bytesSent_; }
    long long audioChunksQueued() const { return audioQueued_; }
    long long audioChunksDropped() const { return audioDropped_; }

    // Queues a message for `client` if still active; `completion` runs once fully written.
    bool send(const std::shared_ptr<ClientSession>& client, proto::Bytes data, std::function<void()> completion = {},
              bool isFrame = false);
    void sendAudio(proto::AudioMessage chunk);
    // Returns the client's address, or nullopt without a client.
    std::optional<std::string> sendControl(const proto::ControlCommand& command);
    void updateHeadsetSettings(const Settings& settings);

private:
    void onListenEvent();
    void onClientEvent(const std::shared_ptr<ClientSession>& client);
    void drop(const std::shared_ptr<ClientSession>& client, const std::string& reason);
    void enqueue(const std::shared_ptr<ClientSession>& client, ClientSession::Pending entry);
    void flush(const std::shared_ptr<ClientSession>& client);
    void readAvailable(const std::shared_ptr<ClientSession>& client);
    void handle(const std::shared_ptr<ClientSession>& client, uint8_t type, const proto::Bytes& payload);
    void dropStaleAudio(ClientSession& client, uint64_t now);
    void sendControlOnConnect(const std::shared_ptr<ClientSession>& client);

    EventLoop& loop_;
    uint16_t port_;
    uint32_t maxInFlight_;
    SOCKET listen_ = INVALID_SOCKET;
    WSAEVENT listenEvent_ = WSA_INVALID_EVENT;
    std::shared_ptr<ClientSession> active_;
    std::vector<proto::ControlCommand> headsetCommands_;
    long long bytesSent_ = 0;
    long long audioQueued_ = 0;
    long long audioDropped_ = 0;
};

}  // namespace vr
