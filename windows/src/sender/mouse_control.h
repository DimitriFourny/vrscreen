// Ctrl+Alt+Win + mouse move → ORBIT, + wheel → DISTANCE_FINE, Ctrl+Alt+Win+Shift + wheel →
// SIZE_FINE. Runs on its own thread: the modifier state is polled 60 times a second, and a
// low-level mouse hook is installed only while the modifiers are held (so games never pay for a
// global hook). While orbiting, mouse moves are swallowed (the cursor stays put) and turned into
// deltas; the wheel is swallowed while either gesture is active. Commands are coalesced to at
// most one batch per 1/60 s.
#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "mouse_gestures.h"

namespace vr {

class MouseControl {
public:
    MouseControl(double degreesPerPixel, double scrollSensitivity,
                 std::function<void(std::vector<proto::ControlCommand>)> onCommands)
        : accumulator_(degreesPerPixel, scrollSensitivity), onCommands_(std::move(onCommands)) {}
    ~MouseControl() { stop(); }
    void start();
    void stop();
    void setSensitivity(double degreesPerPixel, double scrollSensitivity);

private:
    void run();
    static LRESULT CALLBACK hookProc(int code, WPARAM wparam, LPARAM lparam);
    bool handle(WPARAM message, const MSLLHOOKSTRUCT& info);

    std::mutex mutex_;
    MouseGestureAccumulator accumulator_;  // guarded by mutex_ (hook and tick run on the same thread)
    std::function<void(std::vector<proto::ControlCommand>)> onCommands_;
    std::thread thread_;
    DWORD threadId_ = 0;
    std::atomic<bool> stopping_{false};
    HHOOK hook_ = nullptr;
    POINT anchor_{};
    GestureMode mode_ = GestureMode::Inactive;
    struct Stats {
        int orbit = 0, yaw = 0, pitch = 0, distance = 0, distanceSum = 0, size = 0, sizeSum = 0;
    } stats_;
    ULONGLONG lastStatsLog_ = 0;
    static MouseControl* instance_;
};

}  // namespace vr
