// Parsing of adb output and the headset selection rules (ports of the Mac HeadsetAdb.swift).
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace vr::adb {

struct Device {
    enum class State { Device, Unauthorized, Authorizing, Offline, NoPermissions, Other };
    std::string serial;
    State state = State::Other;
    std::string model;    // "Quest 3" (adb reports model:Quest_3); empty while unauthorized
    std::string product;  // ro.product.name, e.g. "eureka"
    bool isQuest() const;
    std::string displayName() const { return model.empty() ? "Android device" : model; }
};

// `adb devices -l`; header, daemon messages and blank lines are skipped.
std::vector<Device> parseDevices(const std::string& output);

struct Selection {
    enum class Kind { None, Ready, Unauthorized, Authorizing, Offline, Ambiguous };
    Kind kind = Kind::None;
    Device device;
    int others = 0;  // Ready: other adb devices attached
    int count = 0;   // Ambiguous: ready devices, none a Quest
};

// A ready Quest wins, then a device waiting for authorization, an offline one, one being
// authorized, and finally the only ready device whatever its model. With `preferredSerial`
// only that device counts.
Selection select(const std::vector<Device>& devices, const std::optional<std::string>& preferredSerial);

struct ReverseEntry {
    std::string remote, local;
};
std::vector<ReverseEntry> parseReverseList(const std::string& output);
enum class ReverseDecision { Present, Create, Replace };
ReverseDecision decideReverse(const std::vector<ReverseEntry>& entries, int port, std::string* existingLocal = nullptr);

// `dumpsys package <pkg> | grep versionName` → "0.3.3", or empty = not installed.
std::string parseVersionName(const std::string& output);
// Numeric dotted comparison; nullopt if either has no leading number. <0, 0, >0.
std::optional<int> compareVersions(const std::string& a, const std::string& b);
// `adb install` output: nullopt on success, else the failure reason.
std::optional<std::string> installFailure(const std::string& output);

enum class TravelMode { On, Off, FormatChanged };
// ` * Travel Mode: true|false` line of `dumpsys tracking`.
TravelMode parseTravelMode(const std::string& output);

}  // namespace vr::adb
