#pragma once

#include <cmath>

namespace qm {

// Safety for "Dim surroundings" in a boundaryless app: nothing stops the user from getting up and
// walking around a dimmed (or black) room, so the dim is lifted while they move and comes back
// once they have settled. Fed the head's horizontal position (gravity-aligned LOCAL space, metres)
// every frame; platform-free (host-tested).
//
// Walking = either
//   - the head is more than kMaxDriftMetres (0.6 m) horizontally from where it was when the dim
//     took effect (or was last restored): the user has left their seat, however slowly; or
//   - its horizontal speed (smoothed over ~0.15 s) stays above kWalkSpeed (0.5 m/s) for
//     kWalkSeconds (0.3 s) while it is at least kMinWalkDisplacement (0.35 m) from that spot.
// Settled = horizontal speed below kStillSpeed (0.1 m/s) for kSettleSeconds (2 s); the spot is
// then re-anchored where the head is.
//
// Why these numbers (seated use must never trigger it): a seated lean pivots at the hips with the
// eyes ~0.7 m above them, so even a 25-30 deg lean moves the head ~0.3-0.35 m; a fast head turn
// (~200 deg/s) moves the eye point, ~0.1 m in front of the neck's axis, at ~0.35 m/s for < 0.3 s
// and only ~0.1-0.15 m in total; tracking jitter is millimetres. Walking, even slowly, is
// 0.5-1.4 m/s sustained with 0.5-0.7 m steps: it passes the speed test within the first step and
// the drift test by the second. The displacement gate keeps a quick seated lean (which can briefly
// exceed 0.5 m/s) from counting as walking.
class WalkingDetector {
public:
    static constexpr double kMaxDriftMetres = 0.6;
    static constexpr double kWalkSpeed = 0.5;            // m/s
    static constexpr double kWalkSeconds = 0.3;
    static constexpr double kMinWalkDisplacement = 0.35;  // m from the spot, for the speed test
    static constexpr double kStillSpeed = 0.1;           // m/s
    static constexpr double kSettleSeconds = 2.0;
    static constexpr double kSpeedSmoothingSeconds = 0.15;

    enum class Event { None, Walking, Settled };

    // `dimInEffect`: the user wants the room dimmed and it would be (a picture is shown). When it
    // is not, the detector does nothing and forgets its spot: the next dim anchors where the head
    // is then. Returns Walking / Settled on the frames where that changes.
    Event update(double x, double z, double dtSeconds, bool dimInEffect) {
        if (!dimInEffect) {
            reset();
            return Event::None;
        }
        if (!haveSpot_) {
            anchor(x, z);
            return Event::None;
        }
        if (dtSeconds > 0.0) {
            const double step = std::hypot(x - prevX_, z - prevZ_) / dtSeconds;
            const double a = dtSeconds / (kSpeedSmoothingSeconds + dtSeconds);
            speed_ += a * (step - speed_);
        }
        prevX_ = x;
        prevZ_ = z;
        const double drift = std::hypot(x - spotX_, z - spotZ_);
        if (!walking_) {
            fastSeconds_ = speed_ > kWalkSpeed ? fastSeconds_ + dtSeconds : 0.0;
            if (drift > kMaxDriftMetres || (fastSeconds_ >= kWalkSeconds && drift >= kMinWalkDisplacement)) {
                walking_ = true;
                stillSeconds_ = 0.0;
                return Event::Walking;
            }
            return Event::None;
        }
        stillSeconds_ = speed_ < kStillSpeed ? stillSeconds_ + dtSeconds : 0.0;
        if (stillSeconds_ >= kSettleSeconds) {
            walking_ = false;
            anchor(x, z);
            return Event::Settled;
        }
        return Event::None;
    }

    // The tracking origin moved (recenter, LOCAL space change): positions jump, start over.
    void reset() {
        haveSpot_ = false;
        walking_ = false;
        speed_ = fastSeconds_ = stillSeconds_ = 0.0;
    }

    bool walking() const { return walking_; }
    double speed() const { return speed_; }

private:
    void anchor(double x, double z) {
        haveSpot_ = true;
        spotX_ = prevX_ = x;
        spotZ_ = prevZ_ = z;
        speed_ = fastSeconds_ = stillSeconds_ = 0.0;
    }

    bool haveSpot_ = false;
    bool walking_ = false;
    double spotX_ = 0.0, spotZ_ = 0.0;
    double prevX_ = 0.0, prevZ_ = 0.0;
    double speed_ = 0.0;
    double fastSeconds_ = 0.0;
    double stillSeconds_ = 0.0;
};

}  // namespace qm
