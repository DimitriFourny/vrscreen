#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// Protocol v1 framing that has no socket in it (../docs/protocol.md): the hello the sender sends on
// every TCP connection, the UDP discovery datagrams and the AUDIO_CONFIG payload. Kept free of
// platform headers so the host tests can check it.
namespace qm::wire {

constexpr uint8_t kMagic[4] = {'V', 'R', 'S', 'C'};  // 56 52 53 43
constexpr uint16_t kProtocolVersion = 1;
constexpr size_t kHelloSize = 6;  // magic, version u16
constexpr uint8_t kDiscover = 0x01;
constexpr uint8_t kAnnounce = 0x02;

inline uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t readU32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}
inline uint64_t readU64(const uint8_t* p) { return (uint64_t{readU32(p)} << 32) | readU32(p + 4); }

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

// AUDIO_CONFIG (0x04): format u8 (1 = PCM s16le interleaved) | sampleRate u32 | channels u8.
constexpr uint8_t kAudioFormatPcmS16le = 1;
struct AudioFormat {
    uint8_t format = 0;
    uint32_t sampleRate = 0;
    uint8_t channels = 0;
};
inline bool parseAudioConfig(const uint8_t* payload, size_t length, AudioFormat* out) {
    if (length < 6) return false;
    out->format = payload[0];
    out->sampleRate = readU32(payload + 1);
    out->channels = payload[5];
    return true;
}
// What this client plays: s16le, 8-192 kHz, mono or stereo.
inline bool playable(const AudioFormat& f) {
    return f.format == kAudioFormatPcmS16le && f.sampleRate >= 8000 && f.sampleRate <= 192000 &&
           (f.channels == 1 || f.channels == 2);
}

}  // namespace qm::wire
