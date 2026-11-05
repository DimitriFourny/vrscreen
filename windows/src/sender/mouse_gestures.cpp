#include "mouse_gestures.h"

#include <algorithm>
#include <cmath>

namespace vr {

GestureMode gestureModeFrom(bool ctrl, bool alt, bool win, bool shift) {
    if (!(ctrl && alt && win)) return GestureMode::Inactive;
    return shift ? GestureMode::Size : GestureMode::OrbitAndDistance;
}

void MouseGestureAccumulator::addMouseDelta(double dx, double dy, GestureMode mode) {
    if (mode != GestureMode::OrbitAndDistance) return;
    yaw_ += dx * degreesPerPixel_ * 100;
    pitch_ -= dy * degreesPerPixel_ * 100;
}

void MouseGestureAccumulator::addWheel(int wheelDelta, GestureMode mode) {
    const double units = wheelDelta / 120.0 * kUnitsPerNotch * scrollSensitivity_;
    if (mode == GestureMode::OrbitAndDistance) distance_ -= units;
    else if (mode == GestureMode::Size) size_ += units;
}

static int16_t take(double& value) {
    const double whole = std::clamp(std::trunc(value), -32768.0, 32767.0);
    value -= whole;
    return int16_t(whole);
}

std::vector<proto::ControlCommand> MouseGestureAccumulator::flush() {
    std::vector<proto::ControlCommand> commands;
    const int16_t yaw = take(yaw_), pitch = take(pitch_);
    if (yaw || pitch) commands.push_back(proto::ControlCommand::orbit(yaw, pitch));
    if (const int16_t d = take(distance_)) commands.push_back(proto::ControlCommand::distanceFine(d));
    if (const int16_t s = take(size_)) commands.push_back(proto::ControlCommand::sizeFine(s));
    return commands;
}

}  // namespace vr
