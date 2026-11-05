// Wire protocol v1 (docs/protocol.md is the binding contract): framing, message payloads,
// CONTROL commands, UDP discovery, and the FRAME_ACK window bookkeeping.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vr::proto {

using Bytes = std::vector<uint8_t>;

constexpr uint8_t kMagic[4] = {'V', 'R', 'S', 'C'};
constexpr uint16_t kVersion = 1;
constexpr size_t kHelloSize = 6;
constexpr size_t kHeaderSize = 5;
constexpr uint16_t kDefaultPort = 7777;
constexpr uint16_t kDefaultDiscoveryPort = 7778;
constexpr uint32_t kMaxPayload = 64u << 20;

enum MessageType : uint8_t {
    kConfig = 0x01,
    kFrame = 0x02,
    kControl = 0x03,
    kAudioConfig = 0x04,
    kAudio = 0x05,
    kPing = 0x10,
    kPong = 0x11,
    kFrameAck = 0x12,
    kHeadsetStatus = 0x13,
};

enum class Codec : uint8_t { H264 = 1, HEVC = 2 };
const char* codecName(Codec codec);

Bytes hello();
// `type u8 | length u32 | payload`.
Bytes frame(uint8_t type, const uint8_t* payload, size_t size);
inline Bytes frame(uint8_t type, const Bytes& payload) { return frame(type, payload.data(), payload.size()); }

struct ConfigMessage {
    Codec codec = Codec::HEVC;
    uint32_t width = 0;
    uint32_t height = 0;
    Bytes parameterSets;  // Annex-B: HEVC VPS+SPS+PPS or H.264 SPS+PPS
    bool operator==(const ConfigMessage&) const = default;
    Bytes encoded() const;
};

struct FrameMessage {
    bool keyframe = false;
    uint64_t captureMicros = 0;
    Bytes accessUnit;  // Annex-B, keyframes start with the parameter sets
    Bytes encoded() const;
};

struct AudioConfigMessage {
    static constexpr uint8_t kPcmS16LE = 1;
    uint8_t format = kPcmS16LE;
    uint32_t sampleRate = 48000;
    uint8_t channels = 2;
    bool operator==(const AudioConfigMessage&) const = default;
    Bytes encoded() const;
};

struct AudioMessage {
    uint64_t captureMicros = 0;  // time of the first sample
    Bytes pcm;                   // s16le interleaved
    Bytes encoded() const;
};

Bytes pongMessage(uint64_t clientMicros, uint64_t serverMicros);
Bytes pingMessage(uint64_t clientMicros);
Bytes frameAckMessage(uint32_t framesConsumed);
Bytes headsetStatusMessage(uint8_t battery, uint8_t flags);

// 0x03 CONTROL.
struct ControlCommand {
    enum : uint8_t {
        kRecenter = 0x01,
        kDistance = 0x03,
        kSize = 0x04,
        kReset = 0x05,
        kTranslate = 0x06,
        kOrbit = 0x07,
        kDistanceFine = 0x08,
        kSizeFine = 0x09,
        kSetCurvature = 0x0A,
        kSetDim = 0x0B,
        kSetHandGestures = 0x0C,
    };
    static constexpr uint16_t kPermilleMax = 1000;

    uint8_t command = 0;
    Bytes args;

    static ControlCommand recenter();
    static ControlCommand reset();
    static ControlCommand distance(int8_t steps);
    static ControlCommand size(int8_t steps);
    static ControlCommand translate(int8_t dx, int8_t dy);
    static ControlCommand orbit(int16_t yawCentidegrees, int16_t pitchCentidegrees);
    static ControlCommand distanceFine(int16_t v);
    static ControlCommand sizeFine(int16_t v);
    static ControlCommand setCurvature(uint16_t permille);
    static ControlCommand setDim(uint16_t permille);
    static ControlCommand setHandGestures(bool on);

    bool operator==(const ControlCommand&) const = default;
    // The CONTROL payload (command byte + args).
    Bytes payload() const;
    // Full framed message.
    Bytes encoded() const { return frame(kControl, payload()); }
    // "TRANSLATE dx=+1 dy=0"; also decodes received payloads (viewer).
    std::string describe() const;
    static std::optional<ControlCommand> decode(const uint8_t* payload, size_t size);
};

// HEADSET_STATUS payload.
struct HeadsetStatus {
    uint8_t battery = 255;  // > 100 = unknown
    uint8_t flags = 0;      // bit 0 charging, bit 1 plugged in
    bool operator==(const HeadsetStatus&) const = default;
    bool known() const { return battery <= 100; }
    bool charging() const { return flags & 1; }
    bool plugged() const { return flags & 2; }
    std::string describe() const;
};

// Incremental parser for a TCP stream. Call `append` with received bytes, then `next` until it
// returns false. Throws nothing: protocol errors set `error()` and stop parsing.
class MessageReader {
public:
    explicit MessageReader(bool expectHello) : awaitingHello_(expectHello) {}
    void append(const uint8_t* data, size_t size);
    // True with a message in `type`/`payload`, false when more bytes are needed or on error.
    bool next(uint8_t& type, Bytes& payload);
    const std::string& error() const { return error_; }
    size_t buffered() const { return buffer_.size() - readIndex_; }

private:
    Bytes buffer_;
    size_t readIndex_ = 0;
    bool awaitingHello_;
    std::string error_;
};

// FRAME_ACK flow control (docs/protocol.md, "Flow control"). Counters wrap like the wire field.
class FrameAckWindow {
public:
    explicit FrameAckWindow(uint32_t limit) : limit_(limit ? limit : 1) {}
    uint32_t limit() const { return limit_; }
    uint32_t framesSent() const { return sent_; }
    uint32_t framesAcked() const { return acked_; }
    uint32_t inFlight() const { return sent_ - acked_; }
    bool active() const { return active_; }
    bool open() const { return !active_ || inFlight() < limit_; }
    void recordSent() { ++sent_; }
    // False (nothing changed) if the count goes backwards or exceeds the frames sent.
    bool recordAck(uint32_t consumed) {
        if (consumed - acked_ > inFlight()) return false;
        acked_ = consumed;
        active_ = true;
        return true;
    }

private:
    uint32_t limit_;
    uint32_t sent_ = 0;
    uint32_t acked_ = 0;
    bool active_ = false;
};

// UDP discovery.
namespace discovery {
constexpr uint8_t kDiscover = 0x01;
constexpr uint8_t kAnnounce = 0x02;
Bytes discover();
// Name cut to at most 63 bytes at a UTF-8 character boundary.
Bytes announce(uint16_t tcpPort, const std::string& name);
bool isDiscover(const uint8_t* data, size_t size);
struct Announce {
    uint16_t tcpPort = 0;
    std::string name;
};
std::optional<Announce> parseAnnounce(const uint8_t* data, size_t size);
}  // namespace discovery

}  // namespace vr::proto
