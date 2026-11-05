// Global shortcuts Ctrl+Alt+Win (+Shift) + arrows / R / 0 (RegisterHotKey: no permission needed,
// exclusive, so a combination another app owns is reported instead of shared). Held keys repeat
// after 300 ms, every 80 ms. UI thread only (the tray window receives WM_HOTKEY / WM_TIMER).
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "protocol.h"

namespace vr {

struct HotkeyBinding {
    UINT vk;
    bool shift;
    const char* label;
    proto::ControlCommand command;
    bool repeats;
};

const std::vector<HotkeyBinding>& hotkeyBindings();

class HotkeyController {
public:
    static constexpr int kFirstId = 0x5652;  // 'VR'
    static constexpr UINT_PTR kRepeatTimer = 0x5653;

    explicit HotkeyController(std::function<void(const HotkeyBinding&, bool isRepeat)> onCommand)
        : onCommand_(std::move(onCommand)) {}
    void registerAll(HWND hwnd);
    void unregisterAll();
    bool registered() const { return !ids_.empty(); }
    // From the window procedure; true if handled.
    bool onHotkey(WPARAM id);
    bool onTimer(UINT_PTR timer);

private:
    void stopRepeat();
    std::function<void(const HotkeyBinding&, bool)> onCommand_;
    HWND hwnd_ = nullptr;
    std::vector<int> ids_;
    int held_ = -1;
    bool firstRepeat_ = false;
};

}  // namespace vr
