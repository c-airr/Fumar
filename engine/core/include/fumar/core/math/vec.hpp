#pragma once

#include "fumar/core/types.hpp"

#include <cmath>

// ---------------------------------------------------------------------------
// Vectors.
//
// These are plain aggregates: no constructors, no private members, no virtuals.
// That keeps them trivially copyable and standard layout, which matters because
// they are memcpy'd straight into GPU buffers. It also enables C++20 designated
// initialisers, so `Vec3{.x = 1.0f, .z = 2.0f}` is valid and self-documenting.
//
// Operators are free functions rather than members: it keeps the type itself an
// aggregate, and it makes `2.0f * v` work exactly like `v * 2.0f`.
// ---------------------------------------------------------------------------

namespace fumar {

struct Vec2 {
    f32 x = 0.0f;
    f32 y = 0.0f;

    friend constexpr bool operator==(const Vec2&, const Vec2&) = default;
};

struct Vec3 {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;

    // Indexed access, written as a switch rather than `(&x)[i]`. Walking a
    // pointer across separate members is undefined behaviour even though it
    // happens to work; the switch is fully constexpr and optimises away.
    constexpr f32& operator[](usize i) {
        switch (i) {
        case 0: return x;
        case 1: return y;
        default: return z;
        }
    }

    constexpr const f32& operator[](usize i) const {
        switch (i) {
        case 0: return x;
        case 1: return y;
        default: return z;
        }
    }

    friend constexpr bool operator==(const Vec3&, const Vec3&) = default;
};

struct Vec4 {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 0.0f;

    constexpr f32& operator[](usize i) {
        switch (i) {
        case 0: return x;
        case 1: return y;
        case 2: return z;
        default: return w;
        }
    }

    constexpr const f32& operator[](usize i) const {
        switch (i) {
        case 0: return x;
        case 1: return y;
        case 2: return z;
        default: return w;
        }
    }

    friend constexpr bool operator==(const Vec4&, const Vec4&) = default;
};

// --- Vec2 -------------------------------------------------------------------

constexpr Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }

constexpr Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }

constexpr Vec2 operator-(Vec2 v) { return {-v.x, -v.y}; }

constexpr Vec2 operator*(Vec2 v, f32 s) { return {v.x * s, v.y * s}; }

constexpr Vec2 operator*(f32 s, Vec2 v) { return v * s; }

constexpr Vec2 operator/(Vec2 v, f32 s) { return {v.x / s, v.y / s}; }

constexpr Vec2& operator+=(Vec2& a, Vec2 b) { return a = a + b; }

constexpr Vec2& operator-=(Vec2& a, Vec2 b) { return a = a - b; }

constexpr Vec2& operator*=(Vec2& v, f32 s) { return v = v * s; }

constexpr f32 dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }

inline f32 length(Vec2 v) { return std::sqrt(dot(v, v)); }

// --- Vec3 -------------------------------------------------------------------

constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }

constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

constexpr Vec3 operator-(Vec3 v) { return {-v.x, -v.y, -v.z}; }

constexpr Vec3 operator*(Vec3 v, f32 s) { return {v.x * s, v.y * s, v.z * s}; }

constexpr Vec3 operator*(f32 s, Vec3 v) { return v * s; }

constexpr Vec3 operator/(Vec3 v, f32 s) { return {v.x / s, v.y / s, v.z / s}; }

constexpr Vec3& operator+=(Vec3& a, Vec3 b) { return a = a + b; }

constexpr Vec3& operator-=(Vec3& a, Vec3 b) { return a = a - b; }

constexpr Vec3& operator*=(Vec3& v, f32 s) { return v = v * s; }

/// Component-wise product. Distinct from `dot` on purpose - this one is for
/// scaling and for multiplying colours together.
constexpr Vec3 hadamard(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }

constexpr f32 dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

/// Right-handed cross product: cross(X, Y) == Z.
constexpr Vec3 cross(Vec3 a, Vec3 b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

constexpr f32 lengthSquared(Vec3 v) { return dot(v, v); }

inline f32 length(Vec3 v) { return std::sqrt(dot(v, v)); }

/// Returns a unit-length copy. A zero vector is returned unchanged rather than
/// producing NaNs - silently degenerate beats silently broken.
inline Vec3 normalize(Vec3 v) {
    const f32 len = length(v);
    return len > 0.0f ? v / len : v;
}

constexpr Vec3 lerp(Vec3 a, Vec3 b, f32 t) { return a + (b - a) * t; }

constexpr Vec3 min(Vec3 a, Vec3 b) {
    return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
}

constexpr Vec3 max(Vec3 a, Vec3 b) {
    return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};
}

// --- Vec4 -------------------------------------------------------------------

constexpr Vec4 operator+(Vec4 a, Vec4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }

constexpr Vec4 operator-(Vec4 a, Vec4 b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }

constexpr Vec4 operator-(Vec4 v) { return {-v.x, -v.y, -v.z, -v.w}; }

constexpr Vec4 operator*(Vec4 v, f32 s) { return {v.x * s, v.y * s, v.z * s, v.w * s}; }

constexpr Vec4 operator*(f32 s, Vec4 v) { return v * s; }

constexpr Vec4 operator/(Vec4 v, f32 s) { return {v.x / s, v.y / s, v.z / s, v.w / s}; }

constexpr Vec4& operator+=(Vec4& a, Vec4 b) { return a = a + b; }

constexpr f32 dot(Vec4 a, Vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

inline f32 length(Vec4 v) { return std::sqrt(dot(v, v)); }

// --- conversions ------------------------------------------------------------

/// Extends a position to homogeneous coordinates (w = 1), so that a Mat4
/// translation applies to it.
constexpr Vec4 point(Vec3 v) { return {v.x, v.y, v.z, 1.0f}; }

/// Extends a direction (w = 0), so that translation does NOT apply to it.
constexpr Vec4 direction(Vec3 v) { return {v.x, v.y, v.z, 0.0f}; }

constexpr Vec3 xyz(Vec4 v) { return {v.x, v.y, v.z}; }

} // namespace fumar
