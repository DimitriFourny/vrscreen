#pragma once

// When to ask Horizon to hide the Guardian boundary (XR_META_boundary_visibility), and how to keep
// that allowed while the surroundings are dimmed. Platform-free (host-tested); XrApp makes the
// OpenXR calls.
//
// The runtime only suppresses the boundary while passthrough is rendered: a request made before
// the passthrough layer has been submitted fails with XR_BOUNDARY_VISIBILITY_SUPPRESSION_NOT_ALLOWED_META,
// and the system may show the boundary again later (XrEventDataBoundaryVisibilityChangedMETA),
// e.g. after the app was paused. So: request once a frame with the passthrough layer has been
// submitted, again whenever the boundary comes back or the session becomes visible again, at most
// every kRetrySeconds.
//
// Dimming: the passthrough layer stays submitted at any opacity, but whether a fully transparent
// one (dim 100 %) still counts as "rendered" is up to the runtime. If the runtime refuses or
// drops the suppression while the passthrough is (almost) invisible, the passthrough opacity gets
// a floor of kOpacityFloor (dim capped at ~95 % opacity loss, the room still barely visible) for
// the rest of the session, and the request is retried.
namespace qm {

class BoundarySuppression {
public:
    static constexpr double kRetrySeconds = 2.0;
    static constexpr float kOpacityFloor = 0.05f;

    // The session became visible (start, resume): request again.
    void onVisible() {
        pending_ = true;
        suppressed_ = false;
    }

    // XrEventDataBoundaryVisibilityChangedMETA. `appliedOpacity`: the passthrough opacity now.
    // Returns true if this raised the opacity floor (log it).
    bool onVisibilityChanged(bool suppressed, float appliedOpacity) {
        suppressed_ = suppressed;
        if (suppressed) {
            pending_ = false;
            return false;
        }
        pending_ = true;
        return raiseFloorIfDim(appliedOpacity);
    }

    // Result of xrRequestBoundaryVisibilityMETA(SUPPRESSED). `notAllowed`: the runtime said
    // passthrough is not rendered. Returns true if this raised the opacity floor.
    bool onRequestResult(bool succeeded, bool notAllowed, float appliedOpacity) {
        if (succeeded) {
            pending_ = false;  // the visibility event confirms; a NOT_SUPPRESSED one re-arms
            return false;
        }
        return notAllowed && raiseFloorIfDim(appliedOpacity);
    }

    // Each frame after xrEndFrame: true if a request should be made now.
    bool shouldRequest(double nowSeconds, bool passthroughSubmitted) {
        if (!pending_ || !passthroughSubmitted) return false;
        if (requested_ && nowSeconds - lastRequestSeconds_ < kRetrySeconds) return false;
        requested_ = true;
        lastRequestSeconds_ = nowSeconds;
        return true;
    }

    bool suppressed() const { return suppressed_; }
    // The lowest passthrough opacity the app may use (0 until the runtime objected).
    float opacityFloor() const { return opacityFloor_; }

private:
    bool raiseFloorIfDim(float appliedOpacity) {
        if (appliedOpacity >= kOpacityFloor || opacityFloor_ >= kOpacityFloor) return false;
        opacityFloor_ = kOpacityFloor;
        return true;
    }

    bool pending_ = true;
    bool suppressed_ = false;
    bool requested_ = false;
    double lastRequestSeconds_ = 0.0;
    float opacityFloor_ = 0.0f;
};

}  // namespace qm
