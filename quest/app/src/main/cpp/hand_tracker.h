#pragma once

#include "hand_gestures.h"

#include <openxr/openxr.h>

#include <string>

namespace qm {

// OpenXR hand tracking for gestures: XR_EXT_hand_tracking trackers for both hands, read with
// XR_FB_hand_tracking_aim (aim pose, index pinch, system-gesture bit). Optional: when the runtime,
// the system or the user's settings do not provide it, available() stays false (logged once) and
// the app works without gestures.
class HandTracker {
public:
    HandTracker() = default;
    HandTracker(const HandTracker&) = delete;
    HandTracker& operator=(const HandTracker&) = delete;
    ~HandTracker() { destroyTrackers(); }

    // Creates both trackers for `session` if the instance enabled both extensions and the system
    // supports hand tracking. Returns available().
    bool createTrackers(XrInstance instance, XrSystemId systemId, XrSession session, bool hasHandTracking,
                        bool hasAim);
    // Before xrDestroySession.
    void destroyTrackers();

    bool available() const { return trackers_[0] != XR_NULL_HANDLE && trackers_[1] != XR_NULL_HANDLE; }
    // Why not, when !available().
    const std::string& unavailableReason() const { return reason_; }

    // Both hands ([0] left, [1] right) at `time` in `space`; untracked when unavailable or when a
    // hand is not tracked (controllers in use, out of view).
    void locate(XrSpace space, XrTime time, HandSample samples[2]);

private:
    PFN_xrCreateHandTrackerEXT pfnCreateHandTrackerEXT = nullptr;
    PFN_xrDestroyHandTrackerEXT pfnDestroyHandTrackerEXT = nullptr;
    PFN_xrLocateHandJointsEXT pfnLocateHandJointsEXT = nullptr;
    XrHandTrackerEXT trackers_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::string reason_ = "not initialised";
};

}  // namespace qm
