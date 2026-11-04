#pragma once

#include "grab.h"

#include <openxr/openxr.h>

namespace qm {

// Light, speed-adaptive smoothing of a tracked pose (a "one euro" filter: a low cutoff while the
// pose is nearly still removes jitter, a cutoff rising with speed keeps fast moves responsive).
class PoseFilter {
public:
    struct Params {
        float minCutoffHz;     // cutoff at rest
        float positionBeta;    // extra cutoff (Hz) per m/s
        float rotationBeta;    // extra cutoff (Hz) per rad/s
        float speedCutoffHz;   // smoothing of the speed estimate itself
    };
    // At rest: 3 Hz (time constant 53 ms). Moving the hand at 0.3 m/s or turning it at 1 rad/s
    // raises the cutoff to about 12 Hz (13 ms).
    static constexpr Params kDefault{3.0f, 30.0f, 9.0f, 1.0f};

    PoseFilter() = default;
    explicit PoseFilter(const Params& params) : params_(params) {}

    // Forget the history: the next update() returns its input unchanged.
    void reset() { valid_ = false; }
    const XrPosef& update(const XrPosef& raw, double dtSeconds);

private:
    Params params_ = kDefault;
    bool valid_ = false;
    XrPosef pose_{{0, 0, 0, 1}, {0, 0, 0}};
    float speed_ = 0.0f;         // filtered m/s
    float angularSpeed_ = 0.0f;  // filtered rad/s
};

// One hand as seen by hand tracking this frame (XR_EXT_hand_tracking + XR_FB_hand_tracking_aim).
struct HandSample {
    bool tracked = false;        // joints active and the aim state valid
    bool systemGesture = false;  // XR_HAND_TRACKING_AIM_SYSTEM_GESTURE_BIT_FB (palm towards the face: Meta menu)
    bool pinchBit = false;       // XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB
    float pinchStrength = 0.0f;  // index pinch, 0..1
    XrPosef aim{{0, 0, 0, 1}, {0, 0, 0}};  // aim pose in the app's space
};

struct HandGestureConfig {
    double holdSeconds = 0.4;     // pinch held this long (tracked, no system gesture) starts a grab
    double joinSeconds = 0.2;     // the second hand joins an active hand grab after this long
    double releaseSeconds = 0.15; // a grabbing pinch must stay loosened this long to release
    double lostSeconds = 0.5;     // a grabbing hand untracked this long releases
    float pinchOn = 0.9f;         // strength counted as a firm pinch (or the runtime's pinching bit)
    float pinchOff = 0.5f;        // below this (and no bit) the pinch is open
    float minWidth = 0.5f;        // two-hand resize clamps
    float maxWidth = 5.0f;
    float minHandDistance = 0.03f;  // hands closer than this at two-hand start count as this far apart
};

// Hand-gesture control of the screen, fed one HandSample per hand every frame.
//
// - One hand: an index pinch held for holdSeconds grabs the screen through Grab (rigid follow of
//   the smoothed aim pose: moving or pointing the hand carries/swings the screen).
// - Two hands: when the other hand also pinches (joinSeconds) the screen's width scales with the
//   distance between the hands (clamped), and its centre follows their midpoint (translation only).
//   Letting go with one hand continues as a one-hand grab with the other, re-anchored where the
//   screen is, so nothing jumps.
// - A grabbing hand drives the screen only while its pinch is firm: while it loosens (the aim pose
//   shifts as the fingers open) or is momentarily untracked the screen holds still, and it
//   re-anchors without a jump if the pinch firms up again. It releases after releaseSeconds loose
//   or lostSeconds untracked.
// - The system gesture never starts a grab and ends any grab by that hand. A hand whose grab ended
//   any other way than by opening (lost, system gesture, another grabber), or that pinched while
//   another grabber held the screen, must open before it can grab again.
// - Disabled: everything is ignored and any hand grab ends at once.
class HandGestures {
public:
    enum class Event { None, Started, TwoHandStarted, TwoHandEnded, Released };

    explicit HandGestures(const HandGestureConfig& config = {}) : config_(config) {}

    // `hands`: [0] left, [1] right. `allowStart`: false while another grabber (controller grip)
    // holds the screen. Updates `position` (screen centre) and `width` while grabbing.
    Event update(double dtSeconds, const HandSample hands[2], bool enabled, bool allowStart, XrVector3f* position,
                 float* width);

    bool active() const { return engagedCount() > 0; }
    bool twoHanded() const { return engagedCount() == 2; }
    // The single grabbing hand (0 left, 1 right), or -1.
    int grabbingHand() const;
    // Why the last grab (or second hand) ended: "pinch released", "hand lost", "system gesture", "disabled".
    const char* lastEndReason() const { return endReason_; }

private:
    struct Hand {
        bool tracked = false;
        bool closed = false;   // pinch state with strength hysteresis
        bool firm = false;     // strength >= pinchOn or the pinching bit
        bool blocked = false;  // must open before it can grab
        bool engaged = false;  // holds the screen
        double closedSeconds = 0;  // continuous tracked, unblocked pinch
        double looseSeconds = 0;   // engaged and not firm
        double lostSeconds = 0;    // engaged and untracked
        PoseFilter filter;
        XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};  // smoothed aim pose (valid while tracked)
    };

    int engagedCount() const { return (hands_[0].engaged ? 1 : 0) + (hands_[1].engaged ? 1 : 0); }
    void disengage(Hand& hand, const char* reason, bool block);
    void anchor(XrVector3f position, float width);

    HandGestureConfig config_;
    Hand hands_[2];
    const char* endReason_ = "";
    // Movement anchoring: valid while `anchoredMode_` matches the engaged hands and all of them drive.
    int anchoredMode_ = 0;  // 0 none, 1 one hand (grab_), 2 both
    int anchoredHand_ = -1;
    Grab grab_;
    float startDistance_ = 0.0f;
    float startWidth_ = 0.0f;
    XrVector3f startMid_{0, 0, 0};
    XrVector3f startPosition_{0, 0, 0};
};

}  // namespace qm
