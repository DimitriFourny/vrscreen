#include "settings.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "json.h"
#include "log.h"
#include "util.h"

namespace vr {

const std::vector<int> Settings::kMaxWidthPresets = {3840, 2560, 1920};
const std::vector<int> Settings::kFpsPresets = {120, 90, 60, 30};
const std::vector<double> Settings::kBitratePresets = {15, 30, 50, 80};
const std::vector<int> Settings::kPercentPresets = {0, 10, 25, 50, 75, 100};

std::string formatMbps(double mbps) {
    return mbps == std::floor(mbps) ? std::to_string(int(mbps)) : formatString("%.1f", mbps);
}

static const char* onOff(bool v) { return v ? "on" : "off"; }

std::string Settings::videoSummary() const {
    return formatString("max %d px, %d fps, %s, %s Mbit/s", maxWidth, fps, proto::codecName(codec),
                        formatMbps(bitrateMbps).c_str());
}

std::string Settings::summary() const {
    return formatString(
        "curvature %d %%, dim %d %%, hand gestures %s, audio %s, mute PC %s, video %s, display %s, shortcuts %s, "
        "mouse %s (%g°/px, scroll ×%g), open headset app %s",
        curvature / 10, dim / 10, onOff(handGestures), onOff(audio), onOff(mutePC), videoSummary().c_str(),
        display.empty() ? "primary" : display.c_str(), onOff(hotkeys), onOff(mouseGestures), mouseSensitivity,
        scrollSensitivity, onOff(launchHeadsetApp));
}

Settings settingsFromJson(const std::string& text) {
    Settings s;
    const auto object = json::parseObject(text);
    if (!object) return s;
    const auto number = [&](const char* key, double fallback) {
        auto it = object->find(key);
        if (it == object->end()) return fallback;
        const double* d = std::get_if<double>(&it->second);
        return d && std::isfinite(*d) ? *d : fallback;
    };
    const auto boolean = [&](const char* key, bool fallback) {
        auto it = object->find(key);
        if (it == object->end()) return fallback;
        const bool* b = std::get_if<bool>(&it->second);
        return b ? *b : fallback;
    };
    const auto string = [&](const char* key, const std::string& fallback) {
        auto it = object->find(key);
        if (it == object->end()) return fallback;
        const std::string* str = std::get_if<std::string>(&it->second);
        return str ? *str : fallback;
    };
    const Settings d;
    s.curvature = uint16_t(std::clamp(number("curvature", d.curvature), 0.0, 1000.0));
    s.dim = uint16_t(std::clamp(number("dim", d.dim), 0.0, 1000.0));
    s.handGestures = boolean("handGestures", d.handGestures);
    s.audio = boolean("audio", d.audio);
    s.mutePC = boolean("mutePC", d.mutePC);
    s.maxWidth = int(std::clamp(number("maxWidth", d.maxWidth), 16.0, 16384.0));
    s.fps = int(std::clamp(number("fps", d.fps), 1.0, 240.0));
    const std::string codec = string("codec", "hevc");
    s.codec = (codec == "h264" || codec == "H264") ? proto::Codec::H264 : proto::Codec::HEVC;
    s.bitrateMbps = std::clamp(number("bitrateMbps", d.bitrateMbps), 0.5, 500.0);
    s.display = string("display", "");
    s.hotkeys = boolean("hotkeys", d.hotkeys);
    s.mouseGestures = boolean("mouseGestures", d.mouseGestures);
    s.mouseSensitivity = std::clamp(number("mouseSensitivity", d.mouseSensitivity), 0.001, 10.0);
    s.scrollSensitivity = std::clamp(number("scrollSensitivity", d.scrollSensitivity), 0.01, 100.0);
    s.launchHeadsetApp = boolean("launchHeadsetApp", d.launchHeadsetApp);
    return s;
}

std::string settingsToJson(const Settings& s) {
    json::Object o;
    o["curvature"] = double(s.curvature);
    o["dim"] = double(s.dim);
    o["handGestures"] = s.handGestures;
    o["audio"] = s.audio;
    o["mutePC"] = s.mutePC;
    o["maxWidth"] = double(s.maxWidth);
    o["fps"] = double(s.fps);
    o["codec"] = std::string(s.codec == proto::Codec::H264 ? "h264" : "hevc");
    o["bitrateMbps"] = s.bitrateMbps;
    if (!s.display.empty()) o["display"] = s.display;
    o["hotkeys"] = s.hotkeys;
    o["mouseGestures"] = s.mouseGestures;
    o["mouseSensitivity"] = s.mouseSensitivity;
    o["scrollSensitivity"] = s.scrollSensitivity;
    o["launchHeadsetApp"] = s.launchHeadsetApp;
    return json::write(o);
}

Settings SettingsOverrides::applied(Settings base) const {
    for (SettingKey key : keys) {
        switch (key) {
            case SettingKey::MaxWidth: base.maxWidth = values.maxWidth; break;
            case SettingKey::Fps: base.fps = values.fps; break;
            case SettingKey::Codec: base.codec = values.codec; break;
            case SettingKey::Bitrate: base.bitrateMbps = values.bitrateMbps; break;
            case SettingKey::Hotkeys: base.hotkeys = values.hotkeys; break;
            case SettingKey::MouseGestures: base.mouseGestures = values.mouseGestures; break;
            case SettingKey::MouseSensitivity: base.mouseSensitivity = values.mouseSensitivity; break;
            case SettingKey::ScrollSensitivity: base.scrollSensitivity = values.scrollSensitivity; break;
            case SettingKey::LaunchHeadsetApp: base.launchHeadsetApp = values.launchHeadsetApp; break;
            case SettingKey::Audio: base.audio = values.audio; break;
        }
    }
    return base;
}

static std::string readFile(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

SettingsModel::SettingsModel(std::wstring path, SettingsOverrides overrides)
    : path_(std::move(path)), overrides_(std::move(overrides)) {
    stored_ = settingsFromJson(readFile(path_));
}

void SettingsModel::reload() { stored_ = settingsFromJson(readFile(path_)); }

void SettingsModel::saveIfDirty() {
    if (!dirty_) return;
    dirty_ = false;
    const size_t slash = path_.find_last_of(L"\\/");
    if (slash != std::wstring::npos) createDirectories(path_.substr(0, slash));
    const std::wstring temp = path_ + L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << settingsToJson(stored_);
        if (!out) {
            log("settings: could not write " + narrow(temp));
            return;
        }
    }
    if (!MoveFileExW(temp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        log("settings: could not save " + narrow(path_) + ": " + win32ErrorText(GetLastError()));
        return;
    }
    log("settings: " + stored_.summary() + " (saved)");
}

}  // namespace vr
