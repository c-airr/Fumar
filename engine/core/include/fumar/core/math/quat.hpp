#pragma once

#include "fumar/core/math/mat.hpp"
#include "fumar/core/math/vec.hpp"
#include "fumar/core/types.hpp"

#include <cmath>

// ---------------------------------------------------------------------------
// Quaternions.
//
// Four floats that encode a rotation. Compared with Euler angles they do not
// suffer from gimbal lock and they interpolate smoothly (slerp), which is why
// cameras, skeletal animation and physics all store orientation this way and
// only convert to a matrix at the last moment.
//
// Layout is (x, y, z) for the vector part and w for the scalar part - the same
// order GLSL and most engines use, so it can be uploaded to the GPU as a vec4.
// ---------------------------------------------------------------------------

namespace fumar {

struct Quat {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 1.0f; // identity rotation by default

    friend constexpr bool operator==(const Quat&, const Quat&) = default;
};

constexpr Quat identityQuat() { return Quat{}; }

/// Hamilton product. Order matters: `a * b` applies b first, then a - the same
/// reading direction as matrix multiplication.
constexpr Quat operator*(Quat a, Quat b) {
    return Quat{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

constexpr Quat operator*(Quat q, f32 s) { return Quat{q.x * s, q.y * s, q.z * s, q.w * s}; }

constexpr Quat operator+(Quat a, Quat b) { return Quat{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }

constexpr Quat operator-(Quat a, Quat b) { return Quat{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }

constexpr Quat operator-(Quat q) { return Quat{-q.x, -q.y, -q.z, -q.w}; }

constexpr f32 dot(Quat a, Quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

/// For a unit quaternion this is also the inverse rotation.
constexpr Quat conjugate(Quat q) { return Quat{-q.x, -q.y, -q.z, q.w}; }

inline Quat normalize(Quat q) {
    const f32 len = std::sqrt(dot(q, q));
    return len > 0.0f ? q * (1.0f / len) : identityQuat();
}

/// Rotation of `radians` about `axis`, following the right-hand rule.
inline Quat fromAxisAngle(Vec3 axis, f32 radians) {
    const Vec3 a = normalize(axis);
    const f32 half = radians * 0.5f;
    const f32 s = std::sin(half);
    return Quat{a.x * s, a.y * s, a.z * s, std::cos(half)};
}

/// Applies the rotation to a vector, using the sandwich product q * v * q'.
/// The expanded form below is the standard optimisation of that expression.
inline Vec3 rotate(Quat q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

inline Mat4 toMat4(Quat q) {
    const Quat n = normalize(q);
    const f32 xx = n.x * n.x;
    const f32 yy = n.y * n.y;
    const f32 zz = n.z * n.z;
    const f32 xy = n.x * n.y;
    const f32 xz = n.x * n.z;
    const f32 yz = n.y * n.z;
    const f32 wx = n.w * n.x;
    const f32 wy = n.w * n.y;
    const f32 wz = n.w * n.z;

    Mat4 result = identity();
    result.at(0, 0) = 1.0f - 2.0f * (yy + zz);
    result.at(0, 1) = 2.0f * (xy - wz);
    result.at(0, 2) = 2.0f * (xz + wy);

    result.at(1, 0) = 2.0f * (xy + wz);
    result.at(1, 1) = 1.0f - 2.0f * (xx + zz);
    result.at(1, 2) = 2.0f * (yz - wx);

    result.at(2, 0) = 2.0f * (xz - wy);
    result.at(2, 1) = 2.0f * (yz + wx);
    result.at(2, 2) = 1.0f - 2.0f * (xx + yy);
    return result;
}

/// Spherical linear interpolation - constant angular velocity along the
/// shortest arc between the two orientations.
inline Quat slerp(Quat a, Quat b, f32 t) {
    f32 cosTheta = dot(a, b);

    // q and -q describe the same rotation, so flipping one of them when the dot
    // product is negative keeps the interpolation on the short way round.
    if (cosTheta < 0.0f) {
        b = -b;
        cosTheta = -cosTheta;
    }

    // Nearly parallel: sin(theta) approaches zero and the general formula loses
    // precision, so fall back to a straight lerp plus renormalisation.
    constexpr f32 kThreshold = 0.9995f;
    if (cosTheta > kThreshold) {
        return normalize(a + (b - a) * t);
    }

    const f32 theta = std::acos(cosTheta);
    const f32 sinTheta = std::sin(theta);
    const f32 wa = std::sin((1.0f - t) * theta) / sinTheta;
    const f32 wb = std::sin(t * theta) / sinTheta;
    return a * wa + b * wb;
}

} // namespace fumar
