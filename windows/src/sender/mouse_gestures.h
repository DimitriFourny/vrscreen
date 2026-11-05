// Ctrl+Alt+Win + mouse → ORBIT, Ctrl+Alt+Win + wheel → DISTANCE_FINE, Ctrl+Alt+Win+Shift +
// wheel → SIZE_FINE: the pure accumulation part (port of the Mac MouseGestureAccumulator).
#pragma once

#include <vector>

#include "protocol.h"

namespace vr {

enum class GestureMode { Inactive, OrbitAndDistance, Size };

// Exactly Ctrl+Alt+Win → orbit/distance; with Shift → size.
GestureMode gestureModeFrom(bool ctrl, bool alt, bool win, bool shift);

class MouseGestureAccumulator {
public:
    // A wheel notch (WHEEL_DELTA = 120) is a quarter DISTANCE/SIZE step.
    static constexpr double kUnitsPerNotch = 25.0;

    MouseGestureAccumulator(double degreesPerPixel, double scrollSensitivity)
        : degreesPerPixel_(degreesPerPixel), scrollSensitivity_(scrollSensitivity) {}
    void setSensitivity(double degreesPerPixel, double scrollSensitivity) {
        degreesPerPixel_ = degreesPerPixel;
        scrollSensitivity_ = scrollSensitivity;
    }
    // Mouse movement in pixels (dy > 0 = down).
    void addMouseDelta(double dx, double dy, GestureMode mode);
    // Wheel delta as in WM_MOUSEWHEEL (positive = away from the user): closer / bigger.
    void addWheel(int wheelDelta, GestureMode mode);
    // Whole units accumulated since the last flush; fractions carry over.
    std::vector<proto::ControlCommand> flush();
    void discardRemainders() { yaw_ = pitch_ = distance_ = size_ = 0; }

private:
    double degreesPerPixel_;
    double scrollSensitivity_;
    double yaw_ = 0, pitch_ = 0, distance_ = 0, size_ = 0;
};

}  // namespace vr
