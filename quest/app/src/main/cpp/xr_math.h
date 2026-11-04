#pragma once

#include <openxr/openxr.h>

#include <cmath>

// Small vector/quaternion helpers for OpenXR poses (right-handed, Y up, -Z forward).
namespace qm {

inline XrVector3f operator+(const XrVector3f& a, const XrVector3f& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline XrVector3f operator-(const XrVector3f& a, const XrVector3f& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline XrVector3f operator*(const XrVector3f& v, float s) { return {v.x * s, v.y * s, v.z * s}; }

inline float length(const XrVector3f& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

inline XrQuaternionf conjugate(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }

// Rotates v by the unit quaternion q.
inline XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v) {
    // v' = v + w*t + q.xyz x t, with t = 2 * (q.xyz x v)
    const XrVector3f t{2.0f * (q.y * v.z - q.z * v.y), 2.0f * (q.z * v.x - q.x * v.z), 2.0f * (q.x * v.y - q.y * v.x)};
    return {v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
            v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
}

// Orientation of a quad (visible from its +Z side, +Y up) at `position` that faces `viewer`:
// yaw towards the viewer plus pitch towards the viewer's height, never roll, so it stays upright.
inline XrQuaternionf facing(const XrVector3f& position, const XrVector3f& viewer) {
    XrVector3f v = viewer - position;
    const float len = length(v);
    if (len < 1e-4f) return {0.0f, 0.0f, 0.0f, 1.0f};
    v = v * (1.0f / len);
    const float yaw = std::atan2(v.x, v.z);   // rotation about +Y taking +Z to v's horizontal part
    const float pitch = -std::asin(v.y);      // rotation about +X tilting +Z up/down to v
    const XrQuaternionf qy{0.0f, std::sin(yaw / 2.0f), 0.0f, std::cos(yaw / 2.0f)};
    const float sp = std::sin(pitch / 2.0f), cp = std::cos(pitch / 2.0f);
    // qy * qx with qx = {sp, 0, 0, cp}
    return {qy.w * sp, qy.y * cp, -qy.y * sp, qy.w * cp};
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

}  // namespace qm
