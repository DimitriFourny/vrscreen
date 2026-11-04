#pragma once

#include <openxr/openxr.h>

#include <algorithm>
#include <cmath>

// Small vector/quaternion helpers for OpenXR poses (right-handed, Y up, -Z forward).
namespace qm {

inline XrVector3f operator+(const XrVector3f& a, const XrVector3f& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline XrVector3f operator-(const XrVector3f& a, const XrVector3f& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline XrVector3f operator*(const XrVector3f& v, float s) { return {v.x * s, v.y * s, v.z * s}; }

inline float dot(const XrVector3f& a, const XrVector3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline XrVector3f cross(const XrVector3f& a, const XrVector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const XrVector3f& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// Hamilton product: rotating by b, then by a.
inline XrQuaternionf multiply(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// Rotation of `angle` radians about the unit vector `axis`.
inline XrQuaternionf axisAngle(const XrVector3f& axis, float angle) {
    const float s = std::sin(angle / 2.0f);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle / 2.0f)};
}

inline XrQuaternionf conjugate(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }

// Rotates v by the unit quaternion q.
inline XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v) {
    // v' = v + w*t + q.xyz x t, with t = 2 * (q.xyz x v)
    const XrVector3f t{2.0f * (q.y * v.z - q.z * v.y), 2.0f * (q.z * v.x - q.x * v.z), 2.0f * (q.x * v.y - q.y * v.x)};
    return {v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
            v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
}

// Unit quaternion whose rotation maps +X, +Y, +Z to the orthonormal right-handed basis x, y, z.
inline XrQuaternionf fromBasis(const XrVector3f& x, const XrVector3f& y, const XrVector3f& z) {
    const float trace = x.x + y.y + z.z;
    XrQuaternionf q;
    if (trace > 0.0f) {
        const float s = 2.0f * std::sqrt(trace + 1.0f);
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
    } else if (x.x > y.y && x.x > z.z) {
        const float s = 2.0f * std::sqrt(1.0f + x.x - y.y - z.z);
        q = {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (y.y > z.z) {
        const float s = 2.0f * std::sqrt(1.0f + y.y - x.x - z.z);
        q = {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        const float s = 2.0f * std::sqrt(1.0f + z.z - x.x - y.y);
        q = {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
    }
    return q;
}

// When facing() takes the screen's "up" from the viewer's head instead of gravity. Gravity alone
// fails in two cases: (1) a screen near the zenith has no "towards the viewer" heading, so tiny
// head movements would spin it and passing over the top would flip it by 180 deg; (2) a viewer
// lying down: on their back with the screen towards the top of their head, gravity's up reads
// upside down for them, and on their side the picture should lie on its side too. So the up blends
// from gravity to the head's up as the screen's elevation goes from 65 to 85 deg, or as the head
// tilts from 50 to 80 deg away from upright (whichever is further along).
constexpr float kFacingBlendStartElevation = 65.0f * 3.14159265f / 180.0f;
constexpr float kFacingBlendEndElevation = 85.0f * 3.14159265f / 180.0f;
constexpr float kFacingBlendStartHeadTilt = 50.0f * 3.14159265f / 180.0f;
constexpr float kFacingBlendEndHeadTilt = 80.0f * 3.14159265f / 180.0f;

// Orientation of a quad (visible from its +Z side, +Y up) at `position` that faces `viewer`.
// Normally (screen within 65 deg of eye level, head tilted less than 50 deg): yaw towards the
// viewer plus pitch towards the viewer's height, never roll, so it stays upright. Beyond that its
// +Y blends towards `viewerUp` (the head's up vector; see above), which stays well defined
// overhead: for someone lying on their back it points to the top of their head, so the picture
// reads upright for them. A standing viewer looking up at a screen has the same up either way.
inline XrQuaternionf facing(const XrVector3f& position, const XrVector3f& viewer, const XrVector3f& viewerUp) {
    XrVector3f v = viewer - position;
    const float len = length(v);
    if (len < 1e-4f) return {0.0f, 0.0f, 0.0f, 1.0f};
    v = v * (1.0f / len);
    // 0 below the start of a blend range, 1 past its end (cosines decrease with the angle).
    auto ramp = [](float cosine, float startAngle, float endAngle) {
        const float a = std::cos(startAngle), b = std::cos(endAngle);
        const float t = (a - cosine) / (a - b);
        return t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    };
    const float horizontal = std::sqrt(v.x * v.x + v.z * v.z);  // cos(elevation)
    float t = std::max(ramp(horizontal, kFacingBlendStartElevation, kFacingBlendEndElevation),
                       ramp(viewerUp.y, kFacingBlendStartHeadTilt, kFacingBlendEndHeadTilt));
    if (t > 0.0f) {
        t = t * t * (3.0f - 2.0f * t);  // smoothstep
        // Both "up" candidates projected into the screen plane, blended.
        const XrVector3f gravityUp = XrVector3f{0.0f, 1.0f, 0.0f} - v * v.y;
        const XrVector3f headUp = viewerUp - v * dot(viewerUp, v);
        const XrVector3f up = gravityUp * (1.0f - t) + headUp * t;
        const float upLen = length(up);
        // Degenerate only when the head's up points along the line of sight with the screen
        // overhead (looking straight ahead while it is above you): fall back to gravity below.
        if (upLen > 1e-3f) {
            const XrVector3f y = up * (1.0f / upLen);
            return fromBasis(cross(y, v), y, v);
        }
    }
    const float yaw = std::atan2(v.x, v.z);   // rotation about +Y taking +Z to v's horizontal part
    const float pitch = -std::asin(v.y);      // rotation about +X tilting +Z up/down to v
    const XrQuaternionf qy{0.0f, std::sin(yaw / 2.0f), 0.0f, std::cos(yaw / 2.0f)};
    const float sp = std::sin(pitch / 2.0f), cp = std::cos(pitch / 2.0f);
    // qy * qx with qx = {sp, 0, 0, cp}
    return {qy.w * sp, qy.y * cp, -qy.y * sp, qy.w * cp};
}

// Horizontal unit direction "in front of" a head with this orientation, robust to looking straight
// up or down: the yaw of the forward vector with the head's pitch undone (forward and up combined),
// so lying on your back and looking at the ceiling gives the direction of your feet.
inline XrVector3f headingOf(const XrQuaternionf& head) {
    const XrVector3f fwd = rotate(head, {0.0f, 0.0f, -1.0f});
    const XrVector3f up = rotate(head, {0.0f, 1.0f, 0.0f});
    // For a head pitched by p without roll: fwd = (cos p) h + (sin p) Y, up = -(sin p) h + (cos p) Y.
    float x = fwd.x * up.y - up.x * fwd.y, z = fwd.z * up.y - up.z * fwd.y;
    float len = std::sqrt(x * x + z * z);
    if (len < 0.2f) {  // rolled far over (lying on a side): the forward vector's own yaw
        x = fwd.x;
        z = fwd.z;
        len = std::sqrt(x * x + z * z);
    }
    if (len < 1e-3f) return {0.0f, 0.0f, -1.0f};
    return {x / len, 0.0f, z / len};
}

// Slides a quad in its own plane: `right` metres along its +X and `up` metres along its +Y.
inline XrVector3f slideInPlane(const XrPosef& pose, float right, float up) {
    return pose.position + rotate(pose.orientation, {right, 0.0f, 0.0f}) + rotate(pose.orientation, {0.0f, up, 0.0f});
}

// Rotates `position` around `pivot` by `yaw` (radians, + = to the viewer's right, i.e. towards +X
// when looking down -Z) and `pitch` (radians, + = up), keeping its distance from the pivot.
// The elevation is clamped to +-maxElevation so the point never passes over the pivot.
inline XrVector3f orbit(const XrVector3f& position, const XrVector3f& pivot, float yaw, float pitch,
                        float maxElevation) {
    const XrVector3f offset = position - pivot;
    const float distance = length(offset);
    if (distance < 1e-4f) return position;
    const XrVector3f d = offset * (1.0f / distance);
    const float azimuth = std::atan2(d.x, -d.z) + yaw;
    float elevation = std::asin(d.y < -1.0f ? -1.0f : d.y > 1.0f ? 1.0f : d.y) + pitch;
    elevation = elevation < -maxElevation ? -maxElevation : elevation > maxElevation ? maxElevation : elevation;
    return pivot + XrVector3f{std::sin(azimuth) * std::cos(elevation), std::sin(elevation),
                              -std::cos(azimuth) * std::cos(elevation)} * distance;
}

// XR_KHR_composition_layer_cylinder geometry for a curved screen whose centre and facing are those
// of the flat quad `screen` (+Z towards the viewer). The radius is viewDistance / curvature (so
// curvature 1 centres the cylinder on the viewer) and the arc length is `width`; if that arc would
// exceed maxAngle the radius grows instead. The cylinder's pose is its axis point, from which the
// arc's centre lies along -Z: the screen centre moved back by the radius towards the viewer.
struct CylinderGeometry {
    XrPosef pose;
    float radius;
    float centralAngle;
};

inline CylinderGeometry cylinderFor(const XrPosef& screen, float width, float viewDistance, float curvature,
                                    float maxAngle) {
    float radius = viewDistance / curvature;
    float angle = width / radius;
    if (angle > maxAngle) {
        angle = maxAngle;
        radius = width / maxAngle;
    }
    return {{screen.orientation, screen.position + rotate(screen.orientation, {0.0f, 0.0f, radius})}, radius, angle};
}

// Highlight shown behind a grabbed screen: the screen's own geometry (same pose; same cylinder axis
// and radius when curved) grown by `border` metres on every side, so a thin frame shows around it.
struct HighlightGeometry {
    XrExtent2Df quadSize;  // flat: quad size
    float centralAngle;    // curved: cylinder arc (radians), same radius as the screen
    float aspectRatio;     // curved: arc length / height
};

// Border width for a screen `width` metres wide: 2.4 cm on the default 1.8 m screen.
inline float highlightBorder(float width) { return 0.01f + 0.008f * width; }

// `width` x `height`: the screen's size (for a cylinder `width` is the arc length and `radius` its
// radius; pass radius 0 for a flat screen).
inline HighlightGeometry highlightFrame(float width, float height, float radius, float border) {
    const float w = width + 2.0f * border, h = height + 2.0f * border;
    constexpr float kMaxAngle = 2.0f * 3.14159265f;
    float angle = radius > 0.0f ? w / radius : 0.0f;
    if (angle > kMaxAngle) angle = kMaxAngle;
    return {{w, h}, angle, w / h};
}

}  // namespace qm
