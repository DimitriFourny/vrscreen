#include "hand_gestures.h"

#include "xr_math.h"

#include <algorithm>
#include <cmath>

namespace qm {

const XrPosef& PoseFilter::update(const XrPosef& raw, double dtSeconds) {
    if (!valid_) {
        pose_ = raw;
        speed_ = angularSpeed_ = 0.0f;
        valid_ = true;
        return pose_;
    }
    if (dtSeconds <= 0.0) return pose_;
    const float dt = static_cast<float>(dtSeconds);
    // Exponential smoothing factor for a first-order low-pass with this cutoff.
    auto alpha = [dt](float cutoffHz) {
        const float tau = 1.0f / (2.0f * 3.14159265f * cutoffHz);
        return 1.0f / (1.0f + tau / dt);
    };
    const float speedAlpha = alpha(params_.speedCutoffHz);

    const XrVector3f delta = raw.position - pose_.position;
    speed_ += speedAlpha * (length(delta) / dt - speed_);
    pose_.position = pose_.position + delta * alpha(params_.minCutoffHz + params_.positionBeta * speed_);

    XrQuaternionf q = raw.orientation;
    const XrQuaternionf& p = pose_.orientation;
    float d = p.x * q.x + p.y * q.y + p.z * q.z + p.w * q.w;
    if (d < 0.0f) {  // same rotation, other hemisphere: interpolate the short way
        q = {-q.x, -q.y, -q.z, -q.w};
        d = -d;
    }
    const float angle = 2.0f * std::acos(std::min(d, 1.0f));
    angularSpeed_ += speedAlpha * (angle / dt - angularSpeed_);
    const float a = alpha(params_.minCutoffHz + params_.rotationBeta * angularSpeed_);
    XrQuaternionf o{p.x + (q.x - p.x) * a, p.y + (q.y - p.y) * a, p.z + (q.z - p.z) * a, p.w + (q.w - p.w) * a};
    const float n = std::sqrt(o.x * o.x + o.y * o.y + o.z * o.z + o.w * o.w);
    pose_.orientation = {o.x / n, o.y / n, o.z / n, o.w / n};
    return pose_;
}

int HandGestures::grabbingHand() const {
    if (engagedCount() != 1) return -1;
    return hands_[0].engaged ? 0 : 1;
}

void HandGestures::disengage(Hand& hand, const char* reason, bool block) {
    hand.engaged = false;
    hand.closedSeconds = hand.looseSeconds = hand.lostSeconds = 0;
    if (block) hand.blocked = true;
    endReason_ = reason;
}

void HandGestures::anchor(XrVector3f position, float width) {
    const int mode = engagedCount();
    anchoredMode_ = mode;
    anchoredHand_ = grabbingHand();
    if (mode == 1) {
        // A fresh Grab: this update is its rising edge, recording the screen in the hand's frame.
        grab_ = Grab{};
        grab_.update(true, &hands_[anchoredHand_].pose, true, &position);
        return;
    }
    const XrVector3f& a = hands_[0].pose.position;
    const XrVector3f& b = hands_[1].pose.position;
    startDistance_ = std::max(length(a - b), config_.minHandDistance);
    startMid_ = (a + b) * 0.5f;
    startWidth_ = width;
    startPosition_ = position;
}

HandGestures::Event HandGestures::update(double dtSeconds, const HandSample samples[2], bool enabled,
                                         bool allowStart, XrVector3f* position, float* width) {
    const bool wasActive = active(), wasTwo = twoHanded();
    if (!enabled) {
        if (wasActive) endReason_ = "disabled";
        for (Hand& h : hands_) h = Hand{};
        anchoredMode_ = 0;
        return wasActive ? Event::Released : Event::None;
    }

    for (int i = 0; i < 2; ++i) {
        Hand& h = hands_[i];
        const HandSample& s = samples[i];
        h.tracked = s.tracked;
        if (!s.tracked) {
            // The hold must be continuous while tracked; a grabbing hand holds still for a while.
            h.filter.reset();
            h.firm = false;
            h.closedSeconds = 0;
            if (h.engaged) {
                h.lostSeconds += dtSeconds;
                if (h.lostSeconds >= config_.lostSeconds) disengage(h, "hand lost", true);
            }
            continue;
        }
        h.lostSeconds = 0;
        h.pose = h.filter.update(s.aim, dtSeconds);
        h.firm = s.pinchBit || s.pinchStrength >= config_.pinchOn;
        if (h.firm) {
            h.closed = true;
        } else if (s.pinchStrength < config_.pinchOff) {
            h.closed = false;
        }
        if (!h.closed) h.blocked = false;
        if (s.systemGesture) {
            // Palm towards the face: the pinch belongs to the system (Meta / menu button).
            h.firm = false;
            h.closedSeconds = 0;
            if (h.closed) h.blocked = true;
            if (h.engaged) disengage(h, "system gesture", true);
            continue;
        }
        if (h.engaged) {
            h.looseSeconds = h.firm ? 0.0 : h.looseSeconds + dtSeconds;
            if (h.looseSeconds >= config_.releaseSeconds) disengage(h, "pinch released", h.closed);
            continue;
        }
        if (h.closed && !allowStart) h.blocked = true;  // first grabber wins; pinch again afterwards
        h.closedSeconds = h.closed && !h.blocked ? h.closedSeconds + dtSeconds : 0.0;
    }

    // Start (both hands at once if both are ready) or join as the second hand.
    const int engagedBefore = engagedCount();
    const double need = engagedBefore == 0 ? config_.holdSeconds : config_.joinSeconds;
    if (engagedBefore == 1 || (engagedBefore == 0 && allowStart)) {
        for (Hand& h : hands_) {
            if (h.tracked && !h.engaged && h.closed && !h.blocked && h.closedSeconds >= need) {
                h.engaged = true;
                h.looseSeconds = h.lostSeconds = 0;
            }
        }
    }

    // Move: only while every grabbing hand is tracked with a firm pinch; any change of the hands
    // involved, or a pause, re-anchors first so the screen never jumps.
    const int mode = engagedCount();
    bool driving = mode > 0;
    for (const Hand& h : hands_) {
        if (h.engaged && !(h.tracked && h.firm)) driving = false;
    }
    if (!driving || mode != anchoredMode_ || (mode == 1 && grabbingHand() != anchoredHand_)) anchoredMode_ = 0;
    if (driving) {
        if (anchoredMode_ == 0) {
            anchor(*position, *width);
        } else if (mode == 1) {
            grab_.update(true, &hands_[anchoredHand_].pose, true, position);
        } else {
            const XrVector3f& a = hands_[0].pose.position;
            const XrVector3f& b = hands_[1].pose.position;
            const float ratio = length(a - b) / startDistance_;
            *width = std::clamp(startWidth_ * ratio, config_.minWidth, config_.maxWidth);
            *position = startPosition_ + ((a + b) * 0.5f - startMid_);
        }
    }

    const bool isActive = active(), isTwo = twoHanded();
    if (wasActive && !isActive) return Event::Released;
    if (!wasActive && isActive) return Event::Started;
    if (isActive && isTwo != wasTwo) return isTwo ? Event::TwoHandStarted : Event::TwoHandEnded;
    return Event::None;
}

}  // namespace qm
