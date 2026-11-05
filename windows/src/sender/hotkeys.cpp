#include "hotkeys.h"

#include "log.h"

namespace vr {

const std::vector<HotkeyBinding>& hotkeyBindings() {
    using C = proto::ControlCommand;
    static const std::vector<HotkeyBinding> bindings = {
        {VK_LEFT, false, "Ctrl+Alt+Win+Left", C::translate(-1, 0), true},
        {VK_RIGHT, false, "Ctrl+Alt+Win+Right", C::translate(1, 0), true},
        {VK_UP, false, "Ctrl+Alt+Win+Up", C::translate(0, 1), true},
        {VK_DOWN, false, "Ctrl+Alt+Win+Down", C::translate(0, -1), true},
        {VK_UP, true, "Ctrl+Alt+Win+Shift+Up", C::distance(1), true},
        {VK_DOWN, true, "Ctrl+Alt+Win+Shift+Down", C::distance(-1), true},
        {VK_RIGHT, true, "Ctrl+Alt+Win+Shift+Right", C::size(1), true},
        {VK_LEFT, true, "Ctrl+Alt+Win+Shift+Left", C::size(-1), true},
        {'R', false, "Ctrl+Alt+Win+R", C::recenter(), false},
        {'0', false, "Ctrl+Alt+Win+0", C::reset(), false},
    };
    return bindings;
}

void HotkeyController::registerAll(HWND hwnd) {
    unregisterAll();
    hwnd_ = hwnd;
    const auto& bindings = hotkeyBindings();
    std::string failed;
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto& b = bindings[i];
        const UINT mods = MOD_CONTROL | MOD_ALT | MOD_WIN | (b.shift ? MOD_SHIFT : 0) | MOD_NOREPEAT;
        const int id = kFirstId + int(i);
        if (RegisterHotKey(hwnd, id, mods, b.vk)) {
            ids_.push_back(id);
        } else {
            failed += std::string(failed.empty() ? "" : ", ") + b.label +
                      (GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED ? " (taken by another app)" : "");
        }
    }
    logf("hotkeys: registered %zu/%zu (Ctrl+Alt+Win + arrows, +Shift + arrows, R, 0)", ids_.size(), bindings.size());
    if (!failed.empty()) log("hotkeys: could not register " + failed);
}

void HotkeyController::unregisterAll() {
    stopRepeat();
    for (int id : ids_) UnregisterHotKey(hwnd_, id);
    ids_.clear();
}

bool HotkeyController::onHotkey(WPARAM wparam) {
    const int index = int(wparam) - kFirstId;
    const auto& bindings = hotkeyBindings();
    if (index < 0 || index >= int(bindings.size())) return false;
    stopRepeat();
    onCommand_(bindings[size_t(index)], false);
    if (bindings[size_t(index)].repeats) {
        held_ = index;
        firstRepeat_ = true;
        SetTimer(hwnd_, kRepeatTimer, 300, nullptr);
    }
    return true;
}

bool HotkeyController::onTimer(UINT_PTR timer) {
    if (timer != kRepeatTimer) return false;
    if (held_ < 0) {
        KillTimer(hwnd_, kRepeatTimer);
        return true;
    }
    const auto& b = hotkeyBindings()[size_t(held_)];
    const auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    const bool held = down(int(b.vk)) && down(VK_CONTROL) && down(VK_MENU) && (down(VK_LWIN) || down(VK_RWIN)) &&
                      down(VK_SHIFT) == b.shift;
    if (!held) {
        stopRepeat();
        return true;
    }
    if (firstRepeat_) {
        firstRepeat_ = false;
        SetTimer(hwnd_, kRepeatTimer, 80, nullptr);
    }
    onCommand_(b, true);
    return true;
}

void HotkeyController::stopRepeat() {
    if (hwnd_) KillTimer(hwnd_, kRepeatTimer);
    held_ = -1;
}

}  // namespace vr
