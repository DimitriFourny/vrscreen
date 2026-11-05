#pragma once

// When to ask Horizon to hide the Guardian boundary (XR_META_boundary_visibility), and how to keep
// that allowed while the surroundings are dimmed. Platform-free (host-tested); XrApp makes the
// OpenXR calls.
//
// The runtime only suppresses the boundary while passthrough is rendered, and right after a
// (re)start of the session it may not be ready yet even though frames already carry the
// passthrough layer: on this Quest, requests 4 ms and 2 s after a pause/resume failed with
// XR_ERROR_RUNTIME_FAILURE. So:
// - after every (re)start (onSessionStarted) and whenever the boundary comes back
//   (XrEventDataBoundaryVisibilityChangedMETA NOT_SUPPRESSED), a request is pending;
// - it is only made once frames have carried the passthrough layer continuously for
//   kSettleSeconds since that point (any frame without it restarts the wait);
// - a refused or failed request is retried with backoff: kFirstRetrySeconds, doubling, at most
//   kMaxRetrySeconds, for as long as the session shows passthrough; success resets the backoff.
//
// Dimming: the passthrough layer stays submitted at any opacity, but whether a fully transparent
// one (dim 100 %) still counts as "rendered" is up to the runtime. If the runtime refuses or
// drops the suppression while the passthrough is (almost) invisible, the passthrough opacity gets
// a floor of kOpacityFloor (dim capped, the room still barely visible) for the rest of the
// session, and the request is retried.
namespace qm {

class BoundarySuppression {
public:
    static constexpr double kSettleSeconds = 0.5;
    static constexpr double kFirstRetrySeconds = 0.5;
    static constexpr double kMaxRetrySeconds = 4.0;
    static constexpr float kOpacityFloor = 0.05f;

    enum class Outcome { Success, NotAllowed, Failed };

    // xrBeginSession succeeded (start or resume): the passthrough has to be on screen again, for
    // kSettleSeconds, before asking.
    void onSessionStarted() {
        pending_ = true;
        suppressed_ = false;
        passthroughSince_ = -1.0;
        backoff_ = kFirstRetrySeconds;
        nextAttempt_ = 0.0;
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

    // Each frame after xrEndFrame: true if a request should be made now. `passthroughSubmitted`:
    // this frame carried the passthrough layer.
    bool shouldRequest(double nowSeconds, bool passthroughSubmitted) {
        if (!passthroughSubmitted) {
            passthroughSince_ = -1.0;
            return false;
        }
        if (passthroughSince_ < 0.0) passthroughSince_ = nowSeconds;
        if (!pending_ || nowSeconds - passthroughSince_ < kSettleSeconds || nowSeconds < nextAttempt_) return false;
        ++attempts_;
        return true;
    }

    // Result of the request made at `nowSeconds`. Returns true if this raised the opacity floor.
    bool onRequestResult(Outcome outcome, double nowSeconds, float appliedOpacity) {
        if (outcome == Outcome::Success) {
            pending_ = false;  // the visibility event confirms; a NOT_SUPPRESSED one re-arms
            backoff_ = kFirstRetrySeconds;
            nextAttempt_ = 0.0;
            return false;
        }
        nextAttempt_ = nowSeconds + backoff_;
        backoff_ = backoff_ * 2.0 < kMaxRetrySeconds ? backoff_ * 2.0 : kMaxRetrySeconds;
        return outcome == Outcome::NotAllowed && raiseFloorIfDim(appliedOpacity);
    }

    bool suppressed() const { return suppressed_; }
    bool pending() const { return pending_; }
    // When the next retry may happen after a refusal or failure (for the log).
    double nextAttemptAt() const { return nextAttempt_; }
    int attempts() const { return attempts_; }
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
    double passthroughSince_ = -1.0;  // start of the current run of frames with passthrough
    double backoff_ = kFirstRetrySeconds;
    double nextAttempt_ = 0.0;
    int attempts_ = 0;
    float opacityFloor_ = 0.0f;
};

}  // namespace qm
