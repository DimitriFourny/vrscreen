#pragma once

#include <cstdint>

// HEADSET_STATUS (client -> server, protocol message 0x13): the headset's battery, so the Mac can
// show it. Platform-free (host-tested); BatteryMonitor reads the values, StreamClient sends them.
namespace qm {

struct HeadsetStatus {
    static constexpr uint8_t kUnknownPercent = 255;
    static constexpr uint8_t kCharging = 0x01;
    static constexpr uint8_t kPluggedIn = 0x02;

    uint8_t batteryPercent = kUnknownPercent;  // 0..100, or 255
    uint8_t flags = 0;

    bool operator==(const HeadsetStatus& o) const { return batteryPercent == o.batteryPercent && flags == o.flags; }
    bool operator!=(const HeadsetStatus& o) const { return !(*this == o); }

    // Packed for an atomic: bit 16 = known.
    uint32_t pack() const { return 0x10000u | (uint32_t{flags} << 8) | batteryPercent; }
    static bool unpack(uint32_t packed, HeadsetStatus* out) {
        if ((packed & 0x10000u) == 0) return false;
        out->batteryPercent = static_cast<uint8_t>(packed & 0xFF);
        out->flags = static_cast<uint8_t>((packed >> 8) & 0xFF);
        return true;
    }
};

// From the sticky ACTION_BATTERY_CHANGED intent's extras: EXTRA_LEVEL / EXTRA_SCALE (-1 when
// missing), EXTRA_STATUS (BatteryManager.BATTERY_STATUS_CHARGING = 2) and EXTRA_PLUGGED (0 = on
// battery, else AC/USB/wireless/dock).
inline HeadsetStatus headsetStatusFromBattery(int level, int scale, int status, int plugged) {
    constexpr int kStatusCharging = 2;
    HeadsetStatus s;
    if (level >= 0 && scale > 0) {
        const int percent = (level * 100 + scale / 2) / scale;
        s.batteryPercent = static_cast<uint8_t>(percent < 0 ? 0 : percent > 100 ? 100 : percent);
    }
    if (status == kStatusCharging) s.flags |= HeadsetStatus::kCharging;
    if (plugged > 0) s.flags |= HeadsetStatus::kPluggedIn;
    return s;
}

// Payload: batteryPercent u8 | flags u8.
inline void encodeHeadsetStatus(const HeadsetStatus& s, uint8_t out[2]) {
    out[0] = s.batteryPercent;
    out[1] = s.flags;
}

// When to send: first thing on a connection (once the status is known), on every change, and at
// least every 60 s.
class HeadsetStatusReporter {
public:
    static constexpr int64_t kHeartbeatMicros = 60'000'000;

    // New connection: the next known status is sent at once.
    void reset() { sent_ = false; }
    // True if `status` should be sent now; then it counts as sent.
    bool shouldSend(const HeadsetStatus& status, int64_t nowMicros) {
        if (sent_ && status == last_ && nowMicros - lastMicros_ < kHeartbeatMicros) return false;
        sent_ = true;
        last_ = status;
        lastMicros_ = nowMicros;
        return true;
    }

private:
    bool sent_ = false;
    HeadsetStatus last_;
    int64_t lastMicros_ = 0;
};

}  // namespace qm
