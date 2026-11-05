// Host unit tests: hand-gesture state machine, pose filter, facing()/headingOf() near the zenith,
// highlight geometry, protocol hello/discovery framing, HEADSET_STATUS, the walking detector that
// lifts the dim, boundary suppression (the audio playout is in audio_tests.cpp).

#include "boundary_suppression.h"
#include "dim_controller.h"
#include "grab.h"
#include "hand_gestures.h"
#include "headset_status.h"
#include "test_support.h"
#include "walking_detector.h"
#include "wire.h"
#include "xr_math.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace qm;

namespace {

constexpr double kDt = 1.0 / 72.0;
constexpr float kPi = 3.14159265f;
const XrQuaternionf kIdentity{0, 0, 0, 1};

using Event = HandGestures::Event;

// Drives a HandGestures with two scripted hands and a screen.
struct Rig {
    HandGestures gestures;
    HandSample hands[2];
    XrVector3f screen{0.0f, 1.6f, -1.2f};
    float width = 1.8f;
    bool enabled = true;
    bool allowStart = true;
    std::vector<Event> events;

    Rig() {
        for (int i = 0; i < 2; ++i) {
            hands[i].aim = {kIdentity, {i == 0 ? -0.15f : 0.15f, 1.3f, -0.35f}};
        }
    }
    void pinch(int hand, float strength) {
        hands[hand].tracked = true;
        hands[hand].pinchStrength = strength;
        hands[hand].pinchBit = strength >= 0.99f;
    }
    // Runs `seconds` of frames; returns the events seen (None skipped).
    std::vector<Event> run(double seconds) {
        std::vector<Event> seen;
        const int frames = static_cast<int>(std::lround(seconds / kDt));
        for (int f = 0; f < frames; ++f) {
            const Event e = gestures.update(kDt, hands, enabled, allowStart, &screen, &width);
            if (e != Event::None) seen.push_back(e);
        }
        events.insert(events.end(), seen.begin(), seen.end());
        return seen;
    }
};

float distance(const XrVector3f& a, const XrVector3f& b) { return length(a - b); }

// ---- Hold timing ----------------------------------------------------------------------------------

TEST(holdStartsAfterAbout400ms) {
    Rig rig;
    rig.pinch(1, 1.0f);
    CHECK(rig.run(0.38).empty());
    CHECK(!rig.gestures.active());
    const auto events = rig.run(0.05);
    CHECK(events.size() == 1 && events[0] == Event::Started);
    CHECK(rig.gestures.active());
    CHECK(rig.gestures.grabbingHand() == 1);
}

TEST(holdRestartsAfterOpening) {
    Rig rig;
    rig.pinch(1, 1.0f);
    rig.run(0.3);
    rig.pinch(1, 0.1f);  // clearly open
    rig.run(0.05);
    rig.pinch(1, 1.0f);
    rig.run(0.3);
    CHECK(!rig.gestures.active());  // 0.3 s since the re-pinch
    rig.run(0.12);
    CHECK(rig.gestures.active());
}

TEST(strengthHysteresisKeepsTheHoldGoing) {
    Rig rig;
    rig.pinch(1, 1.0f);
    rig.run(0.2);
    rig.pinch(1, 0.7f);  // between pinchOff and pinchOn: still closed
    rig.run(0.25);
    CHECK(rig.gestures.active());
}

TEST(weakPinchNeverStarts) {
    Rig rig;
    rig.pinch(1, 0.8f);  // never reached pinchOn
    rig.run(1.0);
    CHECK(!rig.gestures.active());
}

TEST(untrackedFrameResetsTheHold) {
    Rig rig;
    rig.pinch(1, 1.0f);
    rig.run(0.3);
    rig.hands[1].tracked = false;
    rig.run(kDt);
    rig.hands[1].tracked = true;
    rig.run(0.3);
    CHECK(!rig.gestures.active());
    rig.run(0.15);
    CHECK(rig.gestures.active());
}

// ---- Release hysteresis -------------------------------------------------------------------------

TEST(flickeringPinchDoesNotRelease) {
    Rig rig;
    rig.pinch(1, 1.0f);
    rig.run(0.5);
    CHECK(rig.gestures.active());
    for (int i = 0; i < 5; ++i) {
        rig.pinch(1, 0.0f);
        rig.run(3 * kDt);  // 42 ms dropout
        rig.pinch(1, 1.0f);
        rig.run(0.1);
    }
    CHECK(rig.gestures.active());
    CHECK(rig.events.size() == 1);  // just Started
}

TEST(releaseAfterLoosening) {
    Rig rig;
    rig.pinch(1, 1.0f);
    rig.run(0.5);
    rig.pinch(1, 0.0f);
    CHECK(rig.run(0.1).empty());
    const auto events = rig.run(0.1);
    CHECK(events.size() == 1 && events[0] == Event::Released);
    CHECK(std::string(rig.gestures.lastEndReason()) == "pinch released");
}

TEST(screenHoldsStillWhileThePinchLoosens) {
    Rig rig;
    rig.pinch(1, 1.0f);
    rig.run(0.5);
    const XrVector3f before = rig.screen;
    // Opening fingers shift the aim pose; the screen must not follow that.
    rig.pinch(1, 0.3f);
    rig.hands[1].aim.position.y += 0.05f;
    rig.run(0.25);
    CHECK(!rig.gestures.active());
    CHECK_NEAR(distance(rig.screen, before), 0.0, 1e-6);
}

TEST(refirmingAfterALooseMomentDoesNotJump) {
    Rig rig;
    rig.pinch(1, 1.0f);
    rig.run(0.5);
    const XrVector3f before = rig.screen;
    rig.pinch(1, 0.6f);
    rig.hands[1].aim.position.x += 0.1f;  // moved while loose: not applied
    rig.run(0.1);
    rig.pinch(1, 1.0f);
    rig.run(0.5);  // filter settles at the new spot, re-anchored there
    CHECK(rig.gestures.active());
    CHECK_NEAR(distance(rig.screen, before), 0.0, 1e-3);
    rig.hands[1].aim.position.x += 0.2f;  // moves from here on
    rig.run(0.5);
    CHECK_NEAR(rig.screen.x - before.x, 0.2, 1e-3);
}

// ---- One-hand move --------------------------------------------------------------------------------

TEST(oneHandGrabFollowsRigidly) {
    Rig rig;
    rig.pinch(1, 1.0f);
    rig.run(0.5);
    const XrVector3f start = rig.screen;
    rig.hands[1].aim.position = rig.hands[1].aim.position + XrVector3f{0.1f, 0.2f, -0.05f};
    rig.run(0.5);
    CHECK_NEAR(rig.screen.x - start.x, 0.1, 1e-3);
    CHECK_NEAR(rig.screen.y - start.y, 0.2, 1e-3);
    CHECK_NEAR(rig.screen.z - start.z, -0.05, 1e-3);
    // Pointing 60 degrees up swings the screen about the hand, keeping its distance from it.
    const XrVector3f hand = rig.hands[1].aim.position;
    const float reach = distance(rig.screen, hand);
    rig.hands[1].aim.orientation = axisAngle({1, 0, 0}, 60.0f * kPi / 180.0f);
    rig.run(1.0);
    CHECK_NEAR(distance(rig.screen, hand), reach, 1e-3);
    CHECK(rig.screen.y > hand.y + 0.8f * reach * std::sin(60.0f * kPi / 180.0f) - 0.2f);
    CHECK_NEAR(rig.width, 1.8, 1e-6);  // one hand never resizes
}

// ---- System gesture -------------------------------------------------------------------------------

TEST(systemGestureNeverStartsAGrab) {
    Rig rig;
    rig.pinch(0, 1.0f);
    rig.hands[0].systemGesture = true;
    rig.run(1.0);
    CHECK(!rig.gestures.active());
    // Still pinched after the palm turns away: must open first.
    rig.hands[0].systemGesture = false;
    rig.run(1.0);
    CHECK(!rig.gestures.active());
    rig.pinch(0, 0.0f);
    rig.run(0.1);
    rig.pinch(0, 1.0f);
    rig.run(0.45);
    CHECK(rig.gestures.active());
}

TEST(systemGestureEndsAGrab) {
    Rig rig;
    rig.pinch(0, 1.0f);
    rig.run(0.5);
    CHECK(rig.gestures.active());
    rig.hands[0].systemGesture = true;
    const auto events = rig.run(kDt);
    CHECK(events.size() == 1 && events[0] == Event::Released);
    CHECK(std::string(rig.gestures.lastEndReason()) == "system gesture");
}

// ---- Disabled -------------------------------------------------------------------------------------

TEST(disabledIgnoresEverything) {
    Rig rig;
    rig.enabled = false;
    const XrVector3f start = rig.screen;
    rig.pinch(0, 1.0f);
    rig.pinch(1, 1.0f);
    rig.hands[1].aim.position.y += 0.3f;
    CHECK(rig.run(2.0).empty());
    CHECK(!rig.gestures.active());
    CHECK_NEAR(distance(rig.screen, start), 0.0, 0.0);
    CHECK(rig.width == 1.8f);
}

TEST(disablingEndsAGrabAndNeedsAFreshPinch) {
    Rig rig;
    rig.pinch(1, 1.0f);
    rig.run(0.5);
    rig.enabled = false;
    const auto events = rig.run(kDt);
    CHECK(events.size() == 1 && events[0] == Event::Released);
    CHECK(std::string(rig.gestures.lastEndReason()) == "disabled");
    rig.enabled = true;  // re-enabled while still pinching: the hold starts over
    rig.run(0.3);
    CHECK(!rig.gestures.active());
}

// ---- Lost tracking --------------------------------------------------------------------------------

TEST(briefTrackingLossHoldsThenContinuesWithoutJump) {
    Rig rig;
    rig.pinch(1, 1.0f);
    rig.run(0.5);
    const XrVector3f before = rig.screen;
    rig.hands[1].tracked = false;
    rig.run(0.3);
    CHECK(rig.gestures.active());
    rig.hands[1].tracked = true;
    rig.hands[1].aim.position.z += 0.3f;  // came back somewhere else
    rig.run(0.3);
    CHECK(rig.gestures.active());
    CHECK_NEAR(distance(rig.screen, before), 0.0, 1e-6);
}

TEST(longTrackingLossReleases) {
    Rig rig;
    rig.pinch(1, 1.0f);
    rig.run(0.5);
    rig.hands[1].tracked = false;
    const auto events = rig.run(0.6);
    CHECK(events.size() == 1 && events[0] == Event::Released);
    CHECK(std::string(rig.gestures.lastEndReason()) == "hand lost");
    rig.pinch(1, 1.0f);  // back, still pinched: must open first
    rig.run(1.0);
    CHECK(!rig.gestures.active());
}

// ---- Two hands ------------------------------------------------------------------------------------

TEST(twoHandsScaleWidthWithHandDistance) {
    Rig rig;
    rig.pinch(0, 1.0f);
    rig.pinch(1, 1.0f);
    const auto start = rig.run(0.45);
    CHECK(start.size() == 1 && start[0] == Event::Started);
    CHECK(rig.gestures.twoHanded());
    const XrVector3f centre = rig.screen;
    // 0.30 m apart -> 0.45 m apart: width x 1.5; midpoint unchanged.
    rig.hands[0].aim.position.x -= 0.075f;
    rig.hands[1].aim.position.x += 0.075f;
    rig.run(0.5);
    CHECK_NEAR(rig.width, 1.8 * 1.5, 1e-3);
    CHECK_NEAR(distance(rig.screen, centre), 0.0, 1e-4);
    // Both hands up 10 cm: the centre follows the midpoint.
    rig.hands[0].aim.position.y += 0.1f;
    rig.hands[1].aim.position.y += 0.1f;
    rig.run(0.5);
    CHECK_NEAR(rig.screen.y - centre.y, 0.1, 1e-3);
    CHECK_NEAR(rig.width, 2.7, 1e-3);
}

TEST(twoHandWidthIsClamped) {
    HandGestureConfig config;
    config.minWidth = 0.5f;
    config.maxWidth = 5.0f;
    Rig rig;
    rig.gestures = HandGestures(config);
    rig.pinch(0, 1.0f);
    rig.pinch(1, 1.0f);
    rig.run(0.5);
    rig.hands[0].aim.position.x = -1.0f;
    rig.hands[1].aim.position.x = 1.0f;  // 2.0 m apart: x6.7
    rig.run(0.5);
    CHECK_NEAR(rig.width, 5.0, 1e-6);
    rig.hands[0].aim.position.x = -0.01f;
    rig.hands[1].aim.position.x = 0.01f;
    rig.run(0.5);
    CHECK_NEAR(rig.width, 0.5, 1e-6);
}

TEST(secondHandJoinsAnActiveGrab) {
    Rig rig;
    rig.pinch(1, 1.0f);
    rig.run(0.5);
    CHECK(rig.gestures.active() && !rig.gestures.twoHanded());
    rig.pinch(0, 1.0f);
    CHECK(rig.run(0.15).empty());  // joinSeconds = 0.2
    const auto joined = rig.run(0.07);
    CHECK(joined.size() == 1 && joined[0] == Event::TwoHandStarted);
    CHECK(rig.gestures.twoHanded());
}

TEST(releasingOneHandContinuesWithTheOtherWithoutJump) {
    Rig rig;
    rig.pinch(0, 1.0f);
    rig.pinch(1, 1.0f);
    rig.run(0.5);
    rig.hands[1].aim.position.x += 0.15f;  // 0.30 -> 0.45 m: x1.5
    rig.run(0.5);
    CHECK_NEAR(rig.width, 2.7, 1e-3);
    const XrVector3f before = rig.screen;
    rig.pinch(1, 0.0f);
    const auto ended = rig.run(0.25);
    CHECK(ended.size() == 1 && ended[0] == Event::TwoHandEnded);
    CHECK(rig.gestures.active() && rig.gestures.grabbingHand() == 0);
    CHECK_NEAR(distance(rig.screen, before), 0.0, 1e-4);
    CHECK_NEAR(rig.width, 2.7, 1e-3);  // size kept
    // The remaining hand now moves it rigidly from where it is.
    rig.hands[0].aim.position.y -= 0.2f;
    rig.run(0.5);
    CHECK_NEAR(rig.screen.y - before.y, -0.2, 1e-3);
    CHECK_NEAR(rig.width, 2.7, 1e-3);
}

// ---- Grip / hand exclusivity --------------------------------------------------------------------

TEST(noHandGrabWhileAGripHolds) {
    Rig rig;
    rig.allowStart = false;  // a controller grip holds the screen
    rig.pinch(1, 1.0f);
    rig.run(1.0);
    CHECK(!rig.gestures.active());
    rig.allowStart = true;  // grip released, pinch still held: not a fresh pinch
    rig.run(1.0);
    CHECK(!rig.gestures.active());
    rig.pinch(1, 0.0f);
    rig.run(0.1);
    rig.pinch(1, 1.0f);
    rig.run(0.45);
    CHECK(rig.gestures.active());
}

TEST(gripCannotStartWhileAHandHolds) {
    // XrApp passes !handGestures.active() as the grips' allowStart.
    Rig rig;
    rig.pinch(1, 1.0f);
    rig.run(0.5);
    CHECK(rig.gestures.active());
    Grab grip;
    XrVector3f position = rig.screen;
    const XrPosef pose{kIdentity, {0, 1, 0}};
    CHECK(grip.update(true, &pose, !rig.gestures.active(), &position) == Grab::Event::None);
    CHECK(!grip.active());
}

// ---- Pose filter ----------------------------------------------------------------------------------

TEST(poseFilterConvergesAndDampsJitter) {
    PoseFilter filter;
    XrPosef raw{kIdentity, {0, 0, 0}};
    filter.update(raw, kDt);
    raw.position = {0.1f, 0.0f, 0.0f};
    raw.orientation = axisAngle({0, 1, 0}, 0.3f);
    XrPosef out{};
    for (int i = 0; i < 72; ++i) out = filter.update(raw, kDt);
    CHECK_NEAR(out.position.x, 0.1, 1e-4);
    CHECK_NEAR(out.orientation.y, std::sin(0.15), 1e-4);
    // +-2 mm, +-0.5 deg alternating jitter at rest: the output varies much less.
    float minX = 1, maxX = -1, minY = 1, maxY = -1;
    for (int i = 0; i < 144; ++i) {
        const float s = (i % 2 == 0) ? 1.0f : -1.0f;
        raw.position = {0.1f + 0.002f * s, 0.0f, 0.0f};
        raw.orientation = axisAngle({0, 1, 0}, 0.3f + s * 0.5f * kPi / 180.0f);
        out = filter.update(raw, kDt);
        if (i > 72) {
            minX = std::min(minX, out.position.x);
            maxX = std::max(maxX, out.position.x);
            minY = std::min(minY, out.orientation.y);
            maxY = std::max(maxY, out.orientation.y);
        }
    }
    CHECK(maxX - minX < 0.5f * 0.004f);
    CHECK(maxY - minY < 0.5f * (std::sin(0.15f + 0.25f * kPi / 180.0f) - std::sin(0.15f - 0.25f * kPi / 180.0f)));
}

// ---- facing() / headingOf() -----------------------------------------------------------------------

XrVector3f axisX(const XrQuaternionf& q) { return rotate(q, {1, 0, 0}); }
XrVector3f axisY(const XrQuaternionf& q) { return rotate(q, {0, 1, 0}); }
XrVector3f axisZ(const XrQuaternionf& q) { return rotate(q, {0, 0, 1}); }
XrVector3f normalized(const XrVector3f& v) { return v * (1.0f / length(v)); }

// Screen at `elevationDeg` above the viewer at the origin, towards -Z (past 90: over the top, +Z side).
XrVector3f overhead(float elevationDeg, float distance = 1.2f) {
    const float e = elevationDeg * kPi / 180.0f;
    return {0.0f, distance * std::sin(e), -distance * std::cos(e)};
}

TEST(facingIsUprightBelow65Degrees) {
    const XrVector3f viewer{0, 0, 0};
    const XrVector3f tiltedUp = normalized({0.3f, 0.9f, 0.4f});  // head roll/pitch must not matter here
    for (float e : {-60.0f, -30.0f, 0.0f, 30.0f, 60.0f}) {
        for (float az : {-120.0f, 0.0f, 45.0f}) {
            const XrQuaternionf yaw = axisAngle({0, 1, 0}, az * kPi / 180.0f);
            const XrVector3f p = rotate(yaw, overhead(e));
            const XrQuaternionf q = facing(p, viewer, tiltedUp);
            CHECK_NEAR(dot(axisZ(q), normalized(viewer - p)), 1.0, 1e-4);  // faces the viewer
            CHECK_NEAR(axisX(q).y, 0.0, 1e-4);                              // no roll
            CHECK(axisY(q).y > 0.0f);                                       // upright
        }
    }
}

TEST(facingBlendIsContinuousAt65Degrees) {
    const XrVector3f viewer{0, 0, 0};
    const XrVector3f headUp = normalized({0.4f, 0.2f, 0.9f});
    const XrQuaternionf below = facing(overhead(64.99f), viewer, headUp);
    const XrQuaternionf above = facing(overhead(65.01f), viewer, headUp);
    CHECK(dot(axisY(below), axisY(above)) > 0.9999f);
    CHECK(dot(axisX(below), axisX(above)) > 0.9999f);
}

TEST(facingStandingViewerLookingUpMatchesGravity) {
    // Standing, head tilted back to look at a screen 75 deg up: the head's up is the gravity
    // facing's up, so the blend changes nothing.
    const XrVector3f viewer{0, 0, 0};
    const XrVector3f p = overhead(75.0f);
    const XrVector3f lineOfSight = normalized(p);
    const XrVector3f headUp = normalized(XrVector3f{0, 1, 0} - lineOfSight * lineOfSight.y);
    const XrQuaternionf q = facing(p, viewer, headUp);
    CHECK_NEAR(dot(axisZ(q), normalized(viewer - p)), 1.0, 1e-4);
    CHECK_NEAR(axisX(q).y, 0.0, 1e-4);
    CHECK(axisY(q).z > 0.0f);  // top edge leans towards the viewer
}

TEST(facingLyingViewerPassesOverTheZenithWithoutFlipping) {
    // Lying on the back, feet towards -Z: head up = +Z (top of the head), looking at the ceiling.
    const XrVector3f viewer{0, 0, 0};
    const XrVector3f headUp{0, 0, 1};
    XrQuaternionf previous = facing(overhead(60.0f), viewer, headUp);
    // Upright for the lying viewer: the picture's top points to the top of their head.
    CHECK(axisY(previous).z > 0.5f);
    for (float e = 60.5f; e <= 120.0f; e += 0.5f) {
        const XrVector3f p = overhead(e);
        const XrQuaternionf q = facing(p, viewer, headUp);
        CHECK_NEAR(dot(axisZ(q), normalized(viewer - p)), 1.0, 1e-4);
        CHECK(dot(axisY(q), axisY(previous)) > 0.99f);  // no spin, no 180-degree flip
        CHECK(axisY(q).z > 0.0f);
        previous = q;
    }
    // Exactly overhead: well defined, the picture's top towards the top of the head.
    const XrQuaternionf zenith = facing({0, 1.2f, 0}, viewer, headUp);
    CHECK_NEAR(axisY(zenith).z, 1.0, 1e-4);
    CHECK_NEAR(axisZ(zenith).y, -1.0, 1e-4);
    // Tiny head movements under the zenith no longer spin it.
    const XrQuaternionf nudged = facing({0, 1.2f, 0}, {0.01f, 0, 0.01f}, headUp);
    CHECK(dot(axisY(zenith), axisY(nudged)) > 0.999f);
}

TEST(facingDegenerateZenithStaysValid) {
    // Head up along the line of sight (looking straight ahead, screen straight overhead).
    const XrQuaternionf q = facing({0, 1.2f, 0}, {0, 0, 0}, {0, 1, 0});
    const float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    CHECK_NEAR(n, 1.0, 1e-4);
    CHECK_NEAR(axisZ(q).y, -1.0, 1e-4);
}

TEST(facingLyingOnTheSideTurnsThePictureWithTheHead) {
    // Lying on the right side, looking along -Z: the top of the head points to -X.
    const XrVector3f viewer{0, 0, 0};
    const XrQuaternionf q = facing(overhead(20.0f), viewer, {-1, 0, 0});
    CHECK_NEAR(axisY(q).x, -1.0, 1e-3);
    // Head tilted only 40 deg sideways (upright enough): still upright, no roll.
    const float tilt = 40.0f * kPi / 180.0f;
    const XrQuaternionf upright = facing(overhead(20.0f), viewer, {-std::sin(tilt), std::cos(tilt), 0});
    CHECK_NEAR(axisX(upright).y, 0.0, 1e-5);
}

TEST(headingOfIsRobustToPitch) {
    const XrQuaternionf yaw = axisAngle({0, 1, 0}, 0.5f);
    const XrVector3f level = headingOf(yaw);
    const XrVector3f expected = rotate(yaw, {0, 0, -1});
    CHECK_NEAR(level.x, expected.x, 1e-5);
    CHECK_NEAR(level.z, expected.z, 1e-5);
    for (float pitch : {-85.0f, -60.0f, 30.0f, 60.0f, 89.0f, 90.0f}) {
        const XrQuaternionf head = multiply(yaw, axisAngle({1, 0, 0}, pitch * kPi / 180.0f));
        const XrVector3f h = headingOf(head);
        CHECK_NEAR(h.x, expected.x, 1e-4);
        CHECK_NEAR(h.z, expected.z, 1e-4);
    }
    // Lying on the back with the feet towards -Z: looking at the ceiling, "in front" is the feet.
    const XrQuaternionf lying = axisAngle({1, 0, 0}, 90.0f * kPi / 180.0f);
    CHECK_NEAR(headingOf(lying).z, -1.0, 1e-4);
}

// ---- Highlight geometry ---------------------------------------------------------------------------

TEST(highlightFlatGrowsByTheBorder) {
    const float border = highlightBorder(1.8f);
    CHECK_NEAR(border, 0.0244, 1e-4);
    const HighlightGeometry g = highlightFrame(1.8f, 1.0125f, 0.0f, border);
    CHECK_NEAR(g.quadSize.width, 1.8 + 2 * border, 1e-6);
    CHECK_NEAR(g.quadSize.height, 1.0125 + 2 * border, 1e-6);
}

TEST(highlightCylinderSharesTheScreensRadius) {
    const XrPosef screen{kIdentity, {0, 1.6f, -1.2f}};
    const float width = 1.8f, height = 1.8f * 9.0f / 16.0f;
    const CylinderGeometry c = cylinderFor(screen, width, 1.2f, 0.5f, 2.8f);
    const float border = highlightBorder(width);
    const HighlightGeometry g = highlightFrame(width, height, c.radius, border);
    CHECK_NEAR(g.centralAngle * c.radius, width + 2 * border, 1e-5);  // arc grows by the border
    CHECK_NEAR(g.centralAngle * c.radius / g.aspectRatio, height + 2 * border, 1e-5);
    CHECK(g.centralAngle > c.centralAngle);
    // Fully curved, widest screen: the arc is capped by the screen; the frame stays under 2 pi.
    const CylinderGeometry wide = cylinderFor(screen, 5.0f, 0.5f, 1.0f, 2.8f);
    const HighlightGeometry gw = highlightFrame(5.0f, 5.0f * 9 / 16, wide.radius, highlightBorder(5.0f));
    CHECK(gw.centralAngle > 2.8f && gw.centralAngle < 2.0f * kPi);
    // Absurd border: clamped to a full circle.
    CHECK_NEAR(highlightFrame(1.0f, 1.0f, 0.1f, 1.0f).centralAngle, 2.0 * kPi, 1e-5);
}

// ---- Protocol framing (wire.h) ----------------------------------------------------------------------

TEST(helloNeedsTheVrscMagicAndVersion1) {
    std::string error;
    const uint8_t good[6] = {0x56, 0x52, 0x53, 0x43, 0x00, 0x01};  // "VRSC" v1
    CHECK(wire::checkHello(good, &error));
    const uint8_t otherMagic[6] = {'X', 'Y', 'Z', 'W', 0x00, 0x01};
    CHECK(!wire::checkHello(otherMagic, &error) && error == "bad hello magic");
    const uint8_t v2[6] = {'V', 'R', 'S', 'C', 0x00, 0x02};
    CHECK(!wire::checkHello(v2, &error) && error == "unsupported protocol version 2");
}

TEST(discoverDatagramBytes) {
    const wire::DiscoverDatagram d = wire::discoverDatagram();
    const uint8_t expected[7] = {0x56, 0x52, 0x53, 0x43, 0x01, 0x00, 0x01};
    CHECK(std::memcmp(d.bytes, expected, sizeof(expected)) == 0);
}

TEST(announceParsing) {
    // "VRSC", ANNOUNCE, v1, port 7777, name "My Mac".
    const uint8_t datagram[] = {0x56, 0x52, 0x53, 0x43, 0x02, 0x00, 0x01, 0x1E, 0x61, 6, 'M', 'y', ' ', 'M', 'a', 'c'};
    wire::Announce a;
    CHECK(wire::parseAnnounce(datagram, sizeof(datagram), &a));
    CHECK(a.port == 7777);
    CHECK(a.name == "My Mac");
    // A name length past the end is cut to what arrived.
    CHECK(wire::parseAnnounce(datagram, 13, &a) && a.name == "My ");
    // Another magic, a DISCOVER, another version or a short datagram: ignored.
    uint8_t other[sizeof(datagram)];
    std::memcpy(other, datagram, sizeof(datagram));
    other[0] = 'X';
    CHECK(!wire::parseAnnounce(other, sizeof(other), &a));
    std::memcpy(other, datagram, sizeof(datagram));
    other[4] = wire::kDiscover;
    CHECK(!wire::parseAnnounce(other, sizeof(other), &a));
    other[4] = wire::kAnnounce;
    other[6] = 2;
    CHECK(!wire::parseAnnounce(other, sizeof(other), &a));
    CHECK(!wire::parseAnnounce(datagram, 9, &a));
}

TEST(audioConfigParsing) {
    const uint8_t payload[6] = {0x01, 0x00, 0x00, 0xBB, 0x80, 0x02};  // PCM s16le, 48000 Hz, 2 ch
    wire::AudioFormat f;
    CHECK(wire::parseAudioConfig(payload, sizeof(payload), &f));
    CHECK(f.format == 1 && f.sampleRate == 48000 && f.channels == 2);
    CHECK(wire::playable(f));
    CHECK(!wire::parseAudioConfig(payload, 5, &f));
    f.channels = 6;
    CHECK(!wire::playable(f));
    f.channels = 2;
    f.format = 2;
    CHECK(!wire::playable(f));
}

// ---- HEADSET_STATUS -------------------------------------------------------------------------

TEST(headsetStatusFromBatteryExtras) {
    HeadsetStatus s = headsetStatusFromBattery(87, 100, 2, 2);  // charging over USB
    CHECK(s.batteryPercent == 87);
    CHECK(s.flags == (HeadsetStatus::kCharging | HeadsetStatus::kPluggedIn));
    s = headsetStatusFromBattery(100, 100, 5, 1);  // full, still on AC: plugged, not charging
    CHECK(s.batteryPercent == 100 && s.flags == HeadsetStatus::kPluggedIn);
    s = headsetStatusFromBattery(41, 255, 3, 0);  // other scale, discharging
    CHECK(s.batteryPercent == 16 && s.flags == 0);
    s = headsetStatusFromBattery(-1, -1, -1, 0);  // extras missing
    CHECK(s.batteryPercent == HeadsetStatus::kUnknownPercent && s.flags == 0);
    uint8_t payload[2];
    encodeHeadsetStatus(headsetStatusFromBattery(55, 100, 2, 2), payload);
    CHECK(payload[0] == 55 && payload[1] == 0x03);
    HeadsetStatus unpacked;
    CHECK(!HeadsetStatus::unpack(0, &unpacked));  // no reading yet
    CHECK(HeadsetStatus::unpack(s.pack(), &unpacked) && unpacked == s);
}

TEST(headsetStatusSentOnConnectChangeAndEveryMinute) {
    HeadsetStatusReporter r;
    const HeadsetStatus a = headsetStatusFromBattery(80, 100, 3, 0);
    const HeadsetStatus charging = headsetStatusFromBattery(80, 100, 2, 2);
    const HeadsetStatus lower = headsetStatusFromBattery(79, 100, 3, 0);
    CHECK(r.shouldSend(a, 0));            // first one on the connection
    CHECK(!r.shouldSend(a, 10'000'000));  // unchanged
    CHECK(r.shouldSend(charging, 11'000'000));  // flags changed
    CHECK(r.shouldSend(lower, 12'000'000));     // percent changed
    CHECK(!r.shouldSend(lower, 71'000'000));
    CHECK(r.shouldSend(lower, 72'000'000));     // 60 s heartbeat
    r.reset();                                  // new connection
    CHECK(r.shouldSend(lower, 72'100'000));
}

// ---- Walking detector (boundaryless safety for the dim) ----------------------------------------

// Feeds a head trajectory (x, z as functions of time) at 72 Hz; returns the events with times.
struct WalkRun {
    WalkingDetector detector;
    double t = 0.0;
    std::vector<std::pair<double, WalkingDetector::Event>> events;
    void run(double seconds, const std::function<std::pair<double, double>(double)>& head, bool dim = true) {
        const double end = t + seconds;
        while (t < end) {
            t += kDt;
            const auto [x, z] = head(t);
            const WalkingDetector::Event e = detector.update(x, z, kDt, dim);
            if (e != WalkingDetector::Event::None) events.push_back({t, e});
        }
    }
};

// Smooth 0 -> 1 over [t0, t0 + duration] (cosine ease, peak speed pi/2 / duration).
double ease(double t, double t0, double duration) {
    if (t <= t0) return 0.0;
    if (t >= t0 + duration) return 1.0;
    return 0.5 - 0.5 * std::cos(3.14159265 * (t - t0) / duration);
}

TEST(walkingSeatedMotionNeverTriggers) {
    WalkRun r;
    uint32_t seed = 7;
    auto jitter = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return ((seed >> 8) % 4001) / 1e6 - 0.002;  // +-2 mm tracking noise
    };
    r.run(1.0, [&](double) { return std::make_pair(jitter(), jitter()); });
    // A quick lean forward by 0.33 m in 0.4 s (peak 1.3 m/s), held, then back.
    r.run(3.0, [&](double t) { return std::make_pair(jitter(), -0.33 * (ease(t, 1.2, 0.4) - ease(t, 3.0, 0.5))); });
    // A sideways lean to reach something: 0.3 m in 0.5 s and back.
    r.run(3.0, [&](double t) { return std::make_pair(0.3 * (ease(t, 4.2, 0.5) - ease(t, 5.5, 0.5)), jitter()); });
    // Fast head turns: the eye point, 0.1 m in front of the neck, swings 90 deg in 0.25 s and back.
    r.run(3.0, [&](double t) {
        const double a = 1.5708 * (ease(t, 7.2, 0.25) - ease(t, 8.5, 0.25));
        return std::make_pair(0.1 * std::sin(a), -0.1 * std::cos(a) + 0.1);
    });
    CHECK(r.events.empty());
    CHECK(!r.detector.walking());
}

TEST(walkingTriggersWithinTheFirstStep) {
    WalkRun r;
    r.run(1.0, [](double) { return std::make_pair(0.0, 0.0); });
    r.run(3.0, [](double t) { return std::make_pair(0.0, -1.0 * (t - 1.0)); });  // 1 m/s
    CHECK(!r.events.empty());
    CHECK(r.events[0].second == WalkingDetector::Event::Walking);
    // Speed test: displaced 0.35 m at > 0.5 m/s for 0.3 s -> well before the 0.6 m drift.
    CHECK(r.events[0].first > 1.3 && r.events[0].first < 1.5);
    CHECK(r.detector.walking());
}

TEST(walkingSlowDriftTriggersAtSixtyCentimetres) {
    WalkRun r;
    r.run(6.0, [](double t) { return std::make_pair(0.2 * t, 0.0); });  // 0.2 m/s: shuffling away
    CHECK(r.events.size() == 1);
    CHECK_NEAR(r.events[0].first, 3.0, 0.05);  // 0.6 m
}

TEST(walkingSettlesAfterTwoStillSecondsAndReanchors) {
    WalkRun r;
    r.run(1.0, [](double) { return std::make_pair(0.0, 0.0); });
    r.run(2.0, [](double t) { return std::make_pair(0.0, -std::min(1.0 * (t - 1.0), 2.0)); });  // walk 2 m
    CHECK(r.detector.walking());
    // Still walking around at the new place: no settle.
    r.run(3.0, [](double t) { return std::make_pair(0.3 * std::sin(2.0 * t), -2.0); });
    CHECK(r.detector.walking());
    // Sits down (still): settled after ~2 s of < 0.1 m/s.
    const double stopped = r.t;
    r.run(4.0, [](double) { return std::make_pair(0.0, -2.0); });
    CHECK(r.events.size() == 2 && r.events[1].second == WalkingDetector::Event::Settled);
    CHECK(r.events[1].first - stopped > 2.0 && r.events[1].first - stopped < 2.6);
    // Re-anchored there: a seated lean from the new spot does not trigger.
    const double t0 = r.t;
    r.run(3.0, [t0](double t) { return std::make_pair(0.0, -2.0 - 0.3 * ease(t, t0 + 0.5, 0.4)); });
    CHECK(r.events.size() == 2);
}

TEST(walkingWithoutDimDoesNothing) {
    WalkRun r;
    r.run(5.0, [](double t) { return std::make_pair(0.0, -1.0 * t); }, false);
    CHECK(r.events.empty() && !r.detector.walking());
    // The dim taking effect anchors where the head is then (already 5 m away).
    r.run(2.0, [](double) { return std::make_pair(0.0, -5.0); }, true);
    CHECK(r.events.empty() && !r.detector.walking());
}

TEST(dimLiftsAtOnceWhileWalkingAndFadesBack) {
    DimController d;
    d.setRequested(0.8f);
    float v = 0.0f;
    for (int i = 0; i < 72; ++i) v = d.update(true, true, kDt);
    CHECK_NEAR(v, 0.8, 1e-6);
    // Walking: no grace, fades out within kFadeSeconds x 0.8.
    for (int i = 0; i < 18; ++i) v = d.update(true, false, kDt);  // 0.25 s
    CHECK_NEAR(v, 0.0, 1e-6);
    // Settled: back to 0.8 over the fade.
    for (int i = 0; i < 18; ++i) v = d.update(true, true, kDt);
    CHECK_NEAR(v, 0.8, 1e-6);
}

// ---- Boundary suppression (XR_META_boundary_visibility) ------------------------------------------

TEST(boundaryRequestedOnceThePassthroughIsOnScreen) {
    BoundarySuppression b;
    CHECK(!b.shouldRequest(0.0, false));  // nothing submitted yet: the runtime would refuse
    CHECK(b.shouldRequest(0.1, true));
    CHECK(!b.onRequestResult(true, false, 1.0f));
    CHECK(!b.shouldRequest(0.2, true));   // accepted: no more requests
    CHECK(!b.onVisibilityChanged(true, 1.0f));
    CHECK(b.suppressed());
    CHECK(!b.shouldRequest(10.0, true));
}

TEST(boundaryRerequestedWhenShownAgainOrResumed) {
    BoundarySuppression b;
    CHECK(b.shouldRequest(0.0, true));
    b.onRequestResult(true, false, 1.0f);
    b.onVisibilityChanged(true, 1.0f);
    // The system showed it again (e.g. after the Meta menu): request again, rate-limited.
    CHECK(!b.onVisibilityChanged(false, 1.0f));
    CHECK(b.shouldRequest(5.0, true));
    b.onRequestResult(false, true, 1.0f);    // refused (passthrough not on screen yet)
    CHECK(!b.shouldRequest(6.0, true));      // within kRetrySeconds
    CHECK(b.shouldRequest(7.1, true));
    b.onRequestResult(true, false, 1.0f);
    // Paused and resumed: request again once passthrough is back.
    b.onVisible();
    CHECK(!b.suppressed());
    CHECK(!b.shouldRequest(20.0, false));
    CHECK(b.shouldRequest(20.1, true));
    CHECK(b.opacityFloor() == 0.0f);  // never objected at full opacity: no dim cap
}

TEST(boundaryRefusedWhileFullyDimmedCapsTheDim) {
    BoundarySuppression b;
    CHECK(b.shouldRequest(0.0, true));
    // Refused while the passthrough is invisible (dim 100 %): floor the opacity, retry later.
    CHECK(b.onRequestResult(false, true, 0.0f));
    CHECK_NEAR(b.opacityFloor(), BoundarySuppression::kOpacityFloor, 1e-9);
    CHECK(!b.onRequestResult(false, true, 0.0f));  // already capped: reported once
    CHECK(b.shouldRequest(2.5, true));
    // Dropped later while (almost) invisible: same floor, still only once.
    BoundarySuppression c;
    c.shouldRequest(0.0, true);
    c.onRequestResult(true, false, 0.3f);
    c.onVisibilityChanged(true, 0.3f);
    CHECK(c.onVisibilityChanged(false, 0.02f));
    CHECK_NEAR(c.opacityFloor(), BoundarySuppression::kOpacityFloor, 1e-9);
    // Shown again at a visible opacity: unrelated to the dim, no cap.
    BoundarySuppression d;
    CHECK(!d.onVisibilityChanged(false, 0.5f));
    CHECK(d.opacityFloor() == 0.0f);
}

}  // namespace

int main() {
    for (const testing::TestCase& t : testing::registry()) {
        const int before = testing::failures();
        t.fn();
        std::printf("%s %s\n", testing::failures() == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("%zu tests, %d failed checks\n", testing::registry().size(), testing::failures());
    return testing::failures() == 0 ? 0 : 1;
}
