#include "protocol.h"

#include <cstring>

#include "util.h"

namespace vr::proto {

const char* codecName(Codec codec) { return codec == Codec::H264 ? "H.264" : "HEVC"; }

Bytes hello() {
    Bytes out(kMagic, kMagic + 4);
    putU16(out, kVersion);
    return out;
}

Bytes frame(uint8_t type, const uint8_t* payload, size_t size) {
    Bytes out;
    out.reserve(kHeaderSize + size);
    out.push_back(type);
    putU32(out, uint32_t(size));
    if (size) out.insert(out.end(), payload, payload + size);
    return out;
}

Bytes ConfigMessage::encoded() const {
    Bytes p;
    p.reserve(9 + parameterSets.size());
    putU8(p, uint8_t(codec));
    putU32(p, width);
    putU32(p, height);
    p.insert(p.end(), parameterSets.begin(), parameterSets.end());
    return frame(kConfig, p);
}

Bytes FrameMessage::encoded() const {
    // Built in place: access units can be hundreds of KB.
    Bytes out;
    out.reserve(kHeaderSize + 9 + accessUnit.size());
    out.push_back(kFrame);
    putU32(out, uint32_t(9 + accessUnit.size()));
    putU8(out, keyframe ? 1 : 0);
    putU64(out, captureMicros);
    out.insert(out.end(), accessUnit.begin(), accessUnit.end());
    return out;
}

Bytes AudioConfigMessage::encoded() const {
    Bytes p;
    putU8(p, format);
    putU32(p, sampleRate);
    putU8(p, channels);
    return frame(kAudioConfig, p);
}

Bytes AudioMessage::encoded() const {
    Bytes out;
    out.reserve(kHeaderSize + 8 + pcm.size());
    out.push_back(kAudio);
    putU32(out, uint32_t(8 + pcm.size()));
    putU64(out, captureMicros);
    out.insert(out.end(), pcm.begin(), pcm.end());
    return out;
}

Bytes pongMessage(uint64_t clientMicros, uint64_t serverMicros) {
    Bytes p;
    putU64(p, clientMicros);
    putU64(p, serverMicros);
    return frame(kPong, p);
}

Bytes pingMessage(uint64_t clientMicros) {
    Bytes p;
    putU64(p, clientMicros);
    return frame(kPing, p);
}

Bytes frameAckMessage(uint32_t framesConsumed) {
    Bytes p;
    putU32(p, framesConsumed);
    return frame(kFrameAck, p);
}

Bytes headsetStatusMessage(uint8_t battery, uint8_t flags) {
    Bytes p{battery, flags};
    return frame(kHeadsetStatus, p);
}

// MARK: CONTROL

namespace {
ControlCommand make(uint8_t command, Bytes args = {}) {
    ControlCommand c;
    c.command = command;
    c.args = std::move(args);
    return c;
}
Bytes i16(int16_t v) {
    Bytes b;
    putU16(b, uint16_t(v));
    return b;
}
Bytes u16(uint16_t v) {
    Bytes b;
    putU16(b, v);
    return b;
}
std::string signedText(int v) { return v > 0 ? "+" + std::to_string(v) : std::to_string(v); }
}  // namespace

ControlCommand ControlCommand::recenter() { return make(kRecenter); }
ControlCommand ControlCommand::reset() { return make(kReset); }
ControlCommand ControlCommand::distance(int8_t steps) { return make(kDistance, {uint8_t(steps)}); }
ControlCommand ControlCommand::size(int8_t steps) { return make(kSize, {uint8_t(steps)}); }
ControlCommand ControlCommand::translate(int8_t dx, int8_t dy) { return make(kTranslate, {uint8_t(dx), uint8_t(dy)}); }
ControlCommand ControlCommand::orbit(int16_t yaw, int16_t pitch) {
    Bytes a = i16(yaw);
    Bytes b = i16(pitch);
    a.insert(a.end(), b.begin(), b.end());
    return make(kOrbit, a);
}
ControlCommand ControlCommand::distanceFine(int16_t v) { return make(kDistanceFine, i16(v)); }
ControlCommand ControlCommand::sizeFine(int16_t v) { return make(kSizeFine, i16(v)); }
ControlCommand ControlCommand::setCurvature(uint16_t permille) {
    return make(kSetCurvature, u16(permille > kPermilleMax ? kPermilleMax : permille));
}
ControlCommand ControlCommand::setDim(uint16_t permille) {
    return make(kSetDim, u16(permille > kPermilleMax ? kPermilleMax : permille));
}
ControlCommand ControlCommand::setHandGestures(bool on) { return make(kSetHandGestures, {uint8_t(on ? 1 : 0)}); }

Bytes ControlCommand::payload() const {
    Bytes p{command};
    p.insert(p.end(), args.begin(), args.end());
    return p;
}

std::optional<ControlCommand> ControlCommand::decode(const uint8_t* payload, size_t size) {
    if (size < 1) return std::nullopt;
    ControlCommand c;
    c.command = payload[0];
    c.args.assign(payload + 1, payload + size);
    return c;
}

std::string ControlCommand::describe() const {
    const auto s8 = [&](size_t i) { return int(int8_t(args[i])); };
    const auto s16 = [&](size_t i) { return int(int16_t(getU16(&args[i]))); };
    const auto need = [&](size_t n) { return args.size() >= n; };
    switch (command) {
        case kRecenter: return "RECENTER";
        case kReset: return "RESET";
        case kDistance: return need(1) ? "DISTANCE " + signedText(s8(0)) : "DISTANCE (malformed)";
        case kSize: return need(1) ? "SIZE " + signedText(s8(0)) : "SIZE (malformed)";
        case kTranslate:
            return need(2) ? "TRANSLATE dx=" + signedText(s8(0)) + " dy=" + signedText(s8(1)) : "TRANSLATE (malformed)";
        case kOrbit:
            return need(4) ? formatString("ORBIT yaw=%+.2f° pitch=%+.2f°", s16(0) / 100.0, s16(2) / 100.0)
                           : "ORBIT (malformed)";
        case kDistanceFine: return need(2) ? "DISTANCE_FINE " + signedText(s16(0)) : "DISTANCE_FINE (malformed)";
        case kSizeFine: return need(2) ? "SIZE_FINE " + signedText(s16(0)) : "SIZE_FINE (malformed)";
        case kSetCurvature: return need(2) ? "SET_CURVATURE " + std::to_string(getU16(&args[0])) : "SET_CURVATURE (malformed)";
        case kSetDim: return need(2) ? "SET_DIM " + std::to_string(getU16(&args[0])) : "SET_DIM (malformed)";
        case kSetHandGestures:
            return need(1) ? std::string("SET_HAND_GESTURES ") + (args[0] ? "on" : "off") : "SET_HAND_GESTURES (malformed)";
        default: return formatString("unknown command 0x%02X", command);
    }
}

std::string HeadsetStatus::describe() const {
    if (!known()) return "battery unknown";
    std::string s = std::to_string(battery) + " %";
    if (charging()) s += " charging";
    else if (plugged()) s += " plugged in";
    return s;
}

// MARK: Reader

void MessageReader::append(const uint8_t* data, size_t size) {
    if (readIndex_ > 0 && readIndex_ >= buffer_.size() / 2) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + ptrdiff_t(readIndex_));
        readIndex_ = 0;
    }
    buffer_.insert(buffer_.end(), data, data + size);
}

bool MessageReader::next(uint8_t& type, Bytes& payload) {
    if (!error_.empty()) return false;
    if (awaitingHello_) {
        if (buffered() < kHelloSize) return false;
        const uint8_t* p = buffer_.data() + readIndex_;
        if (std::memcmp(p, kMagic, 4) != 0) {
            error_ = "bad hello magic";
            return false;
        }
        if (getU16(p + 4) != kVersion) {
            error_ = "unsupported protocol version " + std::to_string(getU16(p + 4));
            return false;
        }
        readIndex_ += kHelloSize;
        awaitingHello_ = false;
    }
    if (buffered() < kHeaderSize) return false;
    const uint8_t* p = buffer_.data() + readIndex_;
    const uint32_t length = getU32(p + 1);
    if (length > kMaxPayload) {
        error_ = formatString("message type 0x%02X payload %u bytes exceeds limit", p[0], length);
        return false;
    }
    if (buffered() < kHeaderSize + length) return false;
    type = p[0];
    payload.assign(p + kHeaderSize, p + kHeaderSize + length);
    readIndex_ += kHeaderSize + length;
    return true;
}

// MARK: Discovery

namespace discovery {
Bytes discover() {
    Bytes out(kMagic, kMagic + 4);
    out.push_back(kDiscover);
    putU16(out, kVersion);
    return out;
}

Bytes announce(uint16_t tcpPort, const std::string& name) {
    size_t n = name.size() < 63 ? name.size() : 63;
    // Do not cut a UTF-8 sequence: back up over continuation bytes (10xxxxxx).
    if (n < name.size()) {
        while (n > 0 && (static_cast<uint8_t>(name[n]) & 0xC0) == 0x80) --n;
    }
    Bytes out(kMagic, kMagic + 4);
    out.push_back(kAnnounce);
    putU16(out, kVersion);
    putU16(out, tcpPort);
    out.push_back(uint8_t(n));
    out.insert(out.end(), name.begin(), name.begin() + ptrdiff_t(n));
    return out;
}

bool isDiscover(const uint8_t* data, size_t size) {
    return size >= 7 && std::memcmp(data, kMagic, 4) == 0 && data[4] == kDiscover && getU16(data + 5) == kVersion;
}

std::optional<Announce> parseAnnounce(const uint8_t* data, size_t size) {
    if (size < 10 || std::memcmp(data, kMagic, 4) != 0 || data[4] != kAnnounce || getU16(data + 5) != kVersion)
        return std::nullopt;
    const size_t nameLen = data[9];
    if (nameLen > 63 || size < 10 + nameLen) return std::nullopt;
    Announce a;
    a.tcpPort = getU16(data + 7);
    a.name.assign(reinterpret_cast<const char*>(data + 10), nameLen);
    return a;
}
}  // namespace discovery

}  // namespace vr::proto
