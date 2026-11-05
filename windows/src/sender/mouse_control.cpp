#include "mouse_control.h"

#include <mmsystem.h>

#include "log.h"
#include "util.h"

namespace vr {

MouseControl* MouseControl::instance_ = nullptr;

static GestureMode currentMode() {
    const auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    return gestureModeFrom(down(VK_CONTROL), down(VK_MENU), down(VK_LWIN) || down(VK_RWIN), down(VK_SHIFT));
}

void MouseControl::start() {
    stopping_ = false;
    instance_ = this;
    thread_ = std::thread([this] { run(); });
    log("mouse: Ctrl+Alt+Win + mouse orbits, + wheel changes distance (+Shift: size); hooked only while held");
}

void MouseControl::stop() {
    if (!thread_.joinable()) return;
    stopping_ = true;
    PostThreadMessageW(threadId_, WM_NULL, 0, 0);
    thread_.join();
    instance_ = nullptr;
}

void MouseControl::setSensitivity(double degreesPerPixel, double scrollSensitivity) {
    std::lock_guard lock(mutex_);
    accumulator_.setSensitivity(degreesPerPixel, scrollSensitivity);
}

LRESULT CALLBACK MouseControl::hookProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && instance_ && instance_->handle(wparam, *reinterpret_cast<MSLLHOOKSTRUCT*>(lparam))) return 1;
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

bool MouseControl::handle(WPARAM message, const MSLLHOOKSTRUCT& info) {
    const GestureMode mode = currentMode();
    if (mode == GestureMode::Inactive) return false;
    std::lock_guard lock(mutex_);
    if (message == WM_MOUSEMOVE && mode == GestureMode::OrbitAndDistance) {
        accumulator_.addMouseDelta(info.pt.x - anchor_.x, info.pt.y - anchor_.y, mode);
        return true;  // swallowed: the cursor stays at the anchor
    }
    if (message == WM_MOUSEWHEEL) {
        accumulator_.addWheel(int(short(HIWORD(info.mouseData))), mode);
        return true;
    }
    return false;
}

void MouseControl::run() {
    threadId_ = GetCurrentThreadId();
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);  // a slow hook would lag the mouse
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // create the message queue
    ULONGLONG nextTick = GetTickCount64();
    while (!stopping_) {
        const ULONGLONG now = GetTickCount64();
        const DWORD wait = nextTick > now ? DWORD(nextTick - now) : 0;
        MsgWaitForMultipleObjectsEx(0, nullptr, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        if (GetTickCount64() < nextTick) continue;
        nextTick = GetTickCount64() + 16;

        const GestureMode mode = currentMode();
        if (mode != GestureMode::Inactive && !hook_) {
            // An unassigned key event while Win is down keeps Windows from opening the Start menu
            // when Win is released after a mouse gesture (the same trick as AutoHotkey's mask key).
            INPUT mask[2] = {};
            mask[0].type = mask[1].type = INPUT_KEYBOARD;
            mask[0].ki.wVk = mask[1].ki.wVk = 0xE8;
            mask[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, mask, sizeof(INPUT));
            GetCursorPos(&anchor_);
            hook_ = SetWindowsHookExW(WH_MOUSE_LL, hookProc, GetModuleHandleW(nullptr), 0);
            if (!hook_) log("mouse: SetWindowsHookEx failed: " + win32ErrorText(GetLastError()));
        } else if (mode == GestureMode::Inactive && hook_) {
            UnhookWindowsHookEx(hook_);
            hook_ = nullptr;
        }
        if (mode == GestureMode::OrbitAndDistance && mode_ != GestureMode::OrbitAndDistance) GetCursorPos(&anchor_);
        std::vector<proto::ControlCommand> commands;
        {
            std::lock_guard lock(mutex_);
            commands = accumulator_.flush();
            if (mode == GestureMode::Inactive && mode_ != GestureMode::Inactive) accumulator_.discardRemainders();
        }
        mode_ = mode;
        if (!commands.empty()) {
            for (const auto& c : commands) {
                const auto s16 = [&](size_t i) { return int(int16_t(getU16(&c.args[i]))); };
                if (c.command == proto::ControlCommand::kOrbit) {
                    ++stats_.orbit;
                    stats_.yaw += s16(0);
                    stats_.pitch += s16(2);
                } else if (c.command == proto::ControlCommand::kDistanceFine) {
                    ++stats_.distance;
                    stats_.distanceSum += s16(0);
                } else if (c.command == proto::ControlCommand::kSizeFine) {
                    ++stats_.size;
                    stats_.sizeSum += s16(0);
                }
            }
            onCommands_(std::move(commands));
        }
        if (GetTickCount64() - lastStatsLog_ >= 1000) {
            lastStatsLog_ = GetTickCount64();
            std::string parts;
            if (stats_.orbit)
                parts += formatString("ORBIT ×%d Σ yaw %+.2f° pitch %+.2f°", stats_.orbit, stats_.yaw / 100.0, stats_.pitch / 100.0);
            if (stats_.distance)
                parts += formatString("%sDISTANCE_FINE ×%d Σ %d", parts.empty() ? "" : " | ", stats_.distance, stats_.distanceSum);
            if (stats_.size) parts += formatString("%sSIZE_FINE ×%d Σ %d", parts.empty() ? "" : " | ", stats_.size, stats_.sizeSum);
            if (!parts.empty()) log("mouse: " + parts);
            stats_ = Stats{};
        }
    }
    if (hook_) UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
    timeEndPeriod(1);
}

}  // namespace vr
