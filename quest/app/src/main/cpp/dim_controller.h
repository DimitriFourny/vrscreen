#pragma once

#include <cmath>

namespace qm {

// Perceptual dimming of the passthrough surroundings: dim 0.5 should look about half as bright.
// The runtime blends the passthrough's opacity towards black in linear light, so the opacity is
// (1 - dim)^gamma. Measured on the Quest 3 (screencap brightness of the surroundings at
// 250/500/750): gamma 1.8 gave 76/54/30 %, gamma 2.0 gave 71/46/20 %; 1.9 sits in between.
constexpr float kDimGamma = 1.9f;

inline float passthroughOpacity(float dim, float gamma = kDimGamma) {
    if (dim <= 0.0f) return 1.0f;
    if (dim >= 1.0f) return 0.0f;
    return std::pow(1.0f - dim, gamma);
}

// Decides how dim the surroundings are right now. The Mac sets the wanted dim (SET_DIM), but the
// surroundings are only dimmed while the screen shows a decoded picture: with no picture
// (searching, connecting, waiting for the first frame, decoder restart) the user must see the
// room. A short gap (reconnect, Surface resize) keeps the dim for kGraceSeconds to avoid
// flashing; changes fade linearly over kFadeSeconds for the full 0..1 range.
class DimController {
public:
    static constexpr double kGraceSeconds = 1.5;
    static constexpr double kFadeSeconds = 0.3;

    void setRequested(float dim) { requested_ = dim < 0.0f ? 0.0f : dim > 1.0f ? 1.0f : dim; }
    float requested() const { return requested_; }
    float current() const { return current_; }

    // Undimmed at once, with no grace: used when the session becomes visible again.
    void reset() {
        current_ = 0.0f;
        noPictureSeconds_ = kGraceSeconds;
    }

    // Advances by dt seconds and returns the dim to apply now. `safe` false (the user is walking
    // around: WalkingDetector) fades to undimmed at once, without the grace.
    float update(bool pictureShown, bool safe, double dt) {
        noPictureSeconds_ = pictureShown ? 0.0 : noPictureSeconds_ + dt;
        const float target = safe && (pictureShown || noPictureSeconds_ < kGraceSeconds) ? requested_ : 0.0f;
        const float step = static_cast<float>(dt / kFadeSeconds);
        if (current_ < target) current_ = current_ + step < target ? current_ + step : target;
        if (current_ > target) current_ = current_ - step > target ? current_ - step : target;
        return current_;
    }

private:
    float requested_ = 0.0f;
    float current_ = 0.0f;
    double noPictureSeconds_ = kGraceSeconds;  // starts "long without a picture": undimmed
};

}  // namespace qm
