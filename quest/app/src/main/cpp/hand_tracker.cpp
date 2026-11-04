#include "hand_tracker.h"

#include "log.h"

namespace qm {
namespace {

std::string resultName(XrInstance instance, XrResult result) {
    char buf[XR_MAX_RESULT_STRING_SIZE] = {};
    if (XR_SUCCEEDED(xrResultToString(instance, result, buf))) return buf;
    return "XrResult(" + std::to_string(result) + ")";
}

}  // namespace

bool HandTracker::createTrackers(XrInstance instance, XrSystemId systemId, XrSession session, bool hasHandTracking,
                                 bool hasAim) {
    destroyTrackers();
    if (!hasHandTracking || !hasAim) {
        reason_ = std::string("runtime lacks ") +
                  (!hasHandTracking ? XR_EXT_HAND_TRACKING_EXTENSION_NAME : XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME);
        LOGW("Hand tracking unavailable: %s; hand gestures disabled", reason_.c_str());
        return false;
    }
    XrSystemHandTrackingPropertiesEXT handProps{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
    props.next = &handProps;
    XrResult r = xrGetSystemProperties(instance, systemId, &props);
    if (XR_FAILED(r) || !handProps.supportsHandTracking) {
        reason_ = XR_FAILED(r) ? "xrGetSystemProperties: " + resultName(instance, r)
                               : "the system reports no hand tracking support";
        LOGW("Hand tracking unavailable: %s; hand gestures disabled", reason_.c_str());
        return false;
    }
    auto load = [instance](const char* name, auto* out) {
        return XR_SUCCEEDED(xrGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(out))) &&
               *out != nullptr;
    };
    if (!load("xrCreateHandTrackerEXT", &pfnCreateHandTrackerEXT) ||
        !load("xrDestroyHandTrackerEXT", &pfnDestroyHandTrackerEXT) ||
        !load("xrLocateHandJointsEXT", &pfnLocateHandJointsEXT)) {
        reason_ = "hand tracking functions not found";
        LOGW("Hand tracking unavailable: %s; hand gestures disabled", reason_.c_str());
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        XrHandTrackerCreateInfoEXT info{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        info.hand = i == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        r = pfnCreateHandTrackerEXT(session, &info, &trackers_[i]);
        if (XR_FAILED(r)) {
            // XR_ERROR_FEATURE_UNSUPPORTED: hand tracking switched off in the headset's settings,
            // or the HAND_TRACKING permission missing from the manifest.
            reason_ = "xrCreateHandTrackerEXT: " + resultName(instance, r) +
                      " (hand tracking off in Settings > Movement tracking?)";
            LOGW("Hand tracking unavailable: %s; hand gestures disabled", reason_.c_str());
            trackers_[i] = XR_NULL_HANDLE;
            destroyTrackers();
            return false;
        }
    }
    reason_.clear();
    LOGI("Hand trackers created (%s + %s)", XR_EXT_HAND_TRACKING_EXTENSION_NAME,
         XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME);
    return true;
}

void HandTracker::destroyTrackers() {
    for (XrHandTrackerEXT& tracker : trackers_) {
        if (tracker != XR_NULL_HANDLE && pfnDestroyHandTrackerEXT != nullptr) pfnDestroyHandTrackerEXT(tracker);
        tracker = XR_NULL_HANDLE;
    }
}

void HandTracker::locate(XrSpace space, XrTime time, HandSample samples[2]) {
    for (int i = 0; i < 2; ++i) {
        HandSample& s = samples[i];
        s = HandSample{};
        if (trackers_[i] == XR_NULL_HANDLE) continue;
        XrHandTrackingAimStateFB aim{XR_TYPE_HAND_TRACKING_AIM_STATE_FB};
        XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
        XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
        locations.next = &aim;
        locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
        locations.jointLocations = joints;
        XrHandJointsLocateInfoEXT info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
        info.baseSpace = space;
        info.time = time;
        if (XR_FAILED(pfnLocateHandJointsEXT(trackers_[i], &info, &locations))) continue;
        constexpr XrHandTrackingAimFlagsFB kValid = XR_HAND_TRACKING_AIM_COMPUTED_BIT_FB | XR_HAND_TRACKING_AIM_VALID_BIT_FB;
        s.tracked = locations.isActive && (aim.status & kValid) == kValid;
        if (!s.tracked) continue;
        s.systemGesture = (aim.status & XR_HAND_TRACKING_AIM_SYSTEM_GESTURE_BIT_FB) != 0;
        s.pinchBit = (aim.status & XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB) != 0;
        s.pinchStrength = aim.pinchStrengthIndex;
        s.aim = aim.aimPose;
    }
}

}  // namespace qm
