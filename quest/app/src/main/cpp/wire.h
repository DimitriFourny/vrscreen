#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// Protocol v1 framing that has no socket in it (../docs/protocol.md): the hello the sender sends on
// every TCP connection and the UDP discovery datagrams. Kept free of platform headers so the host
// tests can check it.
namespace qm::wire {

constexpr uint8_t kMagic[4] = {'V', 'R', 'S', 'C'};  // 56 52 53 43
constexpr uint16_t kProtocolVersion = 1;
constexpr size_t kHelloSize = 6;  // magic, version u16
constexpr uint8_t kDiscover = 0x01;
constexpr uint8_t kAnnounce = 0x02;

inline uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

// Checks the 6-byte hello; on failure `error` says why.
inline bool checkHello(const uint8_t* hello, std::string* error) {
    if (std::memcmp(hello, kMagic, sizeof(kMagic)) != 0) {
        *error = "bad hello magic";
        return false;
    }
    if (readU16(hello + 4) != kProtocolVersion) {
        *error = "unsupported protocol version " + std::to_string(readU16(hello + 4));
        return false;
    }
    return true;
}

// DISCOVER datagram: magic, type, version u16.
struct DiscoverDatagram {
    uint8_t bytes[7];
};
inline DiscoverDatagram discoverDatagram() {
    return {{kMagic[0], kMagic[1], kMagic[2], kMagic[3], kDiscover, 0, kProtocolVersion}};
}

// ANNOUNCE datagram: magic, type, version u16, TCP port u16, name length u8, UTF-8 name.
// Returns false for anything else (other magic, type or version, too short).
struct Announce {
    uint16_t port = 0;  // 0: the sender did not say (use the default)
    std::string name;
};
inline bool parseAnnounce(const uint8_t* data, size_t size, Announce* out) {
    if (size < 10 || std::memcmp(data, kMagic, sizeof(kMagic)) != 0 || data[4] != kAnnounce ||
        readU16(data + 5) != kProtocolVersion) {
        return false;
    }
    out->port = readU16(data + 7);
    const size_t nameLen = data[9] < size - 10 ? data[9] : size - 10;
    out->name.assign(reinterpret_cast<const char*>(data + 10), nameLen);
    return true;
}

}  // namespace qm::wire
