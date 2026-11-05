#include "discovery.h"

#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include "log.h"
#include "protocol.h"
#include "util.h"

namespace vr {

DiscoveryResponder::DiscoveryResponder(EventLoop& loop, uint16_t port, uint16_t tcpPort)
    : loop_(loop), port_(port), tcpPort_(tcpPort), name_(computerName()) {}

DiscoveryResponder::~DiscoveryResponder() { stop(); }

std::string DiscoveryResponder::computerName() {
    wchar_t name[256];
    DWORD size = 256;
    if (GetComputerNameExW(ComputerNameDnsHostname, name, &size) && size > 0) return narrow(std::wstring_view(name, size));
    size = 256;
    if (GetComputerNameW(name, &size)) return narrow(std::wstring_view(name, size));
    return "Windows PC";
}

bool DiscoveryResponder::start(int& error) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        error = WSAGetLastError();
        return false;
    }
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        error = WSAGetLastError();
        closesocket(s);
        return false;
    }
    // A failed sendto to a vanished client must not make later recvfrom calls fail.
    BOOL noReset = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &noReset, sizeof noReset, nullptr, 0, &bytes, nullptr, nullptr);
    socket_ = s;
    event_ = WSACreateEvent();
    WSAEventSelect(socket_, event_, FD_READ);
    loop_.addHandle(event_, [this] { onEvent(); });
    logf("discovery: answering on UDP 0.0.0.0:%u as \"%s\"", port_, name_.c_str());
    return true;
}

void DiscoveryResponder::stop() {
    if (socket_ == INVALID_SOCKET) return;
    loop_.removeHandle(event_);
    closesocket(socket_);
    WSACloseEvent(event_);
    socket_ = INVALID_SOCKET;
    event_ = WSA_INVALID_EVENT;
}

void DiscoveryResponder::onEvent() {
    WSANETWORKEVENTS ne{};
    WSAEnumNetworkEvents(socket_, event_, &ne);
    const proto::Bytes announce = proto::discovery::announce(tcpPort_, name_);
    for (;;) {
        uint8_t buffer[2048];
        sockaddr_in from{};
        int fromLen = sizeof from;
        const int n = recvfrom(socket_, reinterpret_cast<char*>(buffer), sizeof buffer, 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n == SOCKET_ERROR) return;  // WSAEWOULDBLOCK: drained
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        const std::string source = formatString("%s:%u", ip, ntohs(from.sin_port));
        if (!proto::discovery::isDiscover(buffer, size_t(n))) {
            logf("discovery: ignored %d-byte datagram from %s", n, source.c_str());
            continue;
        }
        if (sendto(socket_, reinterpret_cast<const char*>(announce.data()), int(announce.size()), 0,
                   reinterpret_cast<sockaddr*>(&from), fromLen) == SOCKET_ERROR) {
            log("discovery: DISCOVER from " + source + ", ANNOUNCE failed: " + win32ErrorText(DWORD(WSAGetLastError())));
        } else {
            log("discovery: DISCOVER from " + source + ", announced");
        }
    }
}

}  // namespace vr
