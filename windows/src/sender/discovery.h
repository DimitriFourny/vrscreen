// Answers DISCOVER datagrams on UDP 0.0.0.0:<port> with a unicast ANNOUNCE (docs/protocol.md,
// "Discovery"). Runs on the event loop.
#pragma once

#include <winsock2.h>

#include <string>

#include "event_loop.h"

namespace vr {

class DiscoveryResponder {
public:
    DiscoveryResponder(EventLoop& loop, uint16_t port, uint16_t tcpPort);
    ~DiscoveryResponder();
    bool start(int& error);
    void stop();
    bool running() const { return socket_ != INVALID_SOCKET; }

    // The computer name sent in ANNOUNCE.
    static std::string computerName();

private:
    void onEvent();

    EventLoop& loop_;
    uint16_t port_;
    uint16_t tcpPort_;
    std::string name_;
    SOCKET socket_ = INVALID_SOCKET;
    WSAEVENT event_ = WSA_INVALID_EVENT;
};

}  // namespace vr
