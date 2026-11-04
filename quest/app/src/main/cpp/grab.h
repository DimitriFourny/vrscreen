#pragma once

#include <openxr/openxr.h>

namespace qm {

// Moves a world-locked object with a tracked "grabber" pose: a controller grip today, possibly a
// hand-tracking pinch later. Feed it the grabber's pose and button state every frame.
//
// While held, the object follows the grabber rigidly: the object's position in the grabber's own
// frame at grab start is kept, so moving the hand translates it and turning the wrist swings it
// around the hand. Orientation is up to the caller (XrApp keeps the screen upright, facing the head).
class Grab {
public:
    enum class Event { None, Started, Released };

    // `pressed`: the grab button/gesture is held. `grabber`: its pose in the app's space, or
    // nullptr when not tracked this frame (a held object then stays put). `allowStart`: false while
    // another grabber holds the object. Updates `position` while held.
    Event update(bool pressed, const XrPosef* grabber, bool allowStart, XrVector3f* position);

    bool active() const { return active_; }

private:
    bool active_ = false;
    bool wasPressed_ = false;
    XrVector3f localOffset_{0, 0, 0};  // object position in the grabber's frame at grab start
};

}  // namespace qm
