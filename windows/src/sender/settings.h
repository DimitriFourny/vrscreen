// Everything the user can change in the tray menu, kept in %APPDATA%\VRScreen\settings.json.
// Curvature, dimming and hand gestures are the source of truth for the headset (sent to every
// client on connect and on every change); the rest is PC-side. Same keys as the Mac sender where
// the setting exists on both.
#pragma once

#include <set>
#include <string>
#include <vector>

#include "protocol.h"

namespace vr {

struct Settings {
    // Headset display (CONTROL messages).
    uint16_t curvature = 0;  // permille
    uint16_t dim = 0;        // permille
    bool handGestures = false;
    // Audio.
    bool audio = true;
    bool mutePC = false;
    // Video.
    int maxWidth = 3840;
    int fps = 60;
    proto::Codec codec = proto::Codec::HEVC;
    double bitrateMbps = 30;
    std::string display;  // GDI device name ("\\.\DISPLAY2"); empty = primary display
    // Controls.
    bool hotkeys = true;
    bool mouseGestures = true;
    double mouseSensitivity = 0.1;   // orbit degrees per pixel of mouse movement
    double scrollSensitivity = 1.0;  // multiplier
    // USB.
    bool launchHeadsetApp = true;

    bool operator==(const Settings&) const = default;

    std::vector<proto::ControlCommand> commands() const {
        return {proto::ControlCommand::setCurvature(curvature), proto::ControlCommand::setDim(dim),
                proto::ControlCommand::setHandGestures(handGestures)};
    }
    std::string summary() const;
    std::string videoSummary() const;

    static const std::vector<int> kMaxWidthPresets;
    static const std::vector<int> kFpsPresets;
    static const std::vector<double> kBitratePresets;
    static const std::vector<int> kPercentPresets;  // curvature / dim menu
};

std::string formatMbps(double mbps);

// JSON round trip. Unknown keys are ignored, missing or invalid ones take the default, numbers
// are clamped (hand-edited files).
Settings settingsFromJson(const std::string& text);
std::string settingsToJson(const Settings& settings);

// Setting keys that command-line flags can override for one session.
enum class SettingKey { MaxWidth, Fps, Codec, Bitrate, Hotkeys, MouseGestures, MouseSensitivity, ScrollSensitivity, LaunchHeadsetApp, Audio };

struct SettingsOverrides {
    Settings values;
    std::set<SettingKey> keys;
    Settings applied(Settings base) const;
};

// The stored settings plus the session overrides. Not thread-safe (UI thread).
class SettingsModel {
public:
    SettingsModel(std::wstring path, SettingsOverrides overrides);
    const std::wstring& path() const { return path_; }
    Settings effective() const { return overrides_.applied(stored_); }
    const Settings& stored() const { return stored_; }
    bool isOverridden(SettingKey key) const { return overrides_.keys.count(key) != 0; }
    // Applies `change` to the stored settings and drops the override of `key` (if any).
    template <typename F>
    void change(F&& change, std::initializer_list<SettingKey> keys = {}) {
        change(stored_);
        for (SettingKey k : keys) overrides_.keys.erase(k);
        dirty_ = true;
    }
    void reload();
    // Writes the file if something changed since the last save.
    void saveIfDirty();

private:
    std::wstring path_;
    SettingsOverrides overrides_;
    Settings stored_;
    bool dirty_ = false;
};

}  // namespace vr
