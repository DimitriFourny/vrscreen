// Command line of vrscreen-sender.exe (mirrors the Mac sender's options where they apply).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "settings.h"

namespace vr {

struct DebugControlStep {
    bool wait = false;
    int waitMs = 0;
    proto::ControlCommand command;
};

struct Options {
    enum class Source { Screen, Test };
    Source source = Source::Screen;
    uint16_t port = proto::kDefaultPort;
    std::optional<uint16_t> discoveryPort = proto::kDefaultDiscoveryPort;
    double keyframeIntervalSeconds = 5;
    int maxInFlight = 3;
    int testWidth = 1920;
    int testHeight = 1080;
    std::optional<int> displayIndex;  // --display N (this session)
    bool menu = true;                 // tray icon
    bool audioAllowed = true;         // false with --no-audio
    int audioOffsetMs = 0;            // added to AUDIO timestamps
    std::optional<std::wstring> settingsPath;
    bool adb = true;
    std::optional<std::string> adbSerial;
    std::vector<DebugControlStep> controlOnConnect;
    bool console = false;  // attach/allocate a console for the log
    bool listDisplays = false;
    bool version = false;
    bool help = false;
    std::string encoderName;  // --encoder substring of the hardware encoder's name
    SettingsOverrides overrides;
};

// Throws std::runtime_error with a message on bad input.
Options parseOptions(const std::vector<std::string>& args);
std::vector<DebugControlStep> parseControlSpec(const std::string& spec);
extern const char* const kUsage;

}  // namespace vr
