#include "grab.h"

#include "xr_math.h"

namespace qm {

Grab::Event Grab::update(bool pressed, const XrPosef* grabber, bool allowStart, XrVector3f* position) {
    const bool risingEdge = pressed && !wasPressed_;
    wasPressed_ = pressed;

    if (!active_) {
        // Start only on a fresh press with a tracked grabber; a press that began untracked or
        // while someone else held the object needs to be released and pressed again.
        if (!risingEdge || grabber == nullptr || !allowStart) return Event::None;
        active_ = true;
        localOffset_ = rotate(conjugate(grabber->orientation), *position - grabber->position);
        return Event::Started;
    }
    if (!pressed) {
        active_ = false;
        return Event::Released;
    }
    if (grabber != nullptr) *position = grabber->position + rotate(grabber->orientation, localOffset_);
    return Event::None;
}

}  // namespace qm
