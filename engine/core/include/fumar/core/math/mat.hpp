#pragma once

#include "fumar/core/math/vec.hpp"
#include "fumar/core/types.hpp"

#include <cmath>

// ---------------------------------------------------------------------------
// 4x4 matrices, stored column-major.
//
// Column-major means `columns[1]` is the second COLUMN of the matrix, not the
// second row. This is not an arbitrary choice: GLSL and SPIR-V lay matrices out
// the same way, so a Mat4 can be memcpy'd into a uniform buffer and read as a
// mat4 in the shader with no transposition step in between.
//
// Multiplication follows maths convention, so `projection * view * model`
// transforms a model-space point into clip space, applied right to left.
// ---------------------------------------------------------------------------

namespace fumar {

struct Mat4 {
    Vec4 columns[4]{};

    /// Element access in (row, column) order, the way a matrix is written down
    /// on paper. The storage is transposed relative to this on purpose.
    constexpr f32& at(usize row, usize col) { return columns[col][row]; }

    constexpr const f32& at(usize row, usize col) const { return columns[col][row]; }

    friend constexpr bool operator==(const Mat4&, const Mat4&) = default;
};

constexpr Mat4 identity() {
    return Mat4{{
        Vec4{1.0f, 0.0f, 0.0f, 0.0f},
        Vec4{0.0f, 1.0f, 0.0f, 0.0f},
        Vec4{0.0f, 0.0f, 1.0f, 0.0f},
        Vec4{0.0f, 0.0f, 0.0f, 1.0f},
    }};
}

/// Transforms a homogeneous vector. Use `point()` / `direction()` from vec.hpp
/// to build the w component correctly.
constexpr Vec4 operator*(const Mat4& m, Vec4 v) {
    return m.columns[0] * v.x + m.columns[1] * v.y + m.columns[2] * v.z + m.columns[3] * v.w;
}

constexpr Mat4 operator*(const Mat4& a, const Mat4& b) {
    return Mat4{{
        a * b.columns[0],
        a * b.columns[1],
        a * b.columns[2],
        a * b.columns[3],
    }};
}

constexpr Mat4 transpose(const Mat4& m) {
    Mat4 result{};
    for (usize col = 0; col < 4; ++col) {
        for (usize row = 0; row < 4; ++row) {
            result.at(row, col) = m.at(col, row);
        }
    }
    return result;
}

// --- affine transforms ------------------------------------------------------

constexpr Mat4 translation(Vec3 t) {
    Mat4 result = identity();
    result.columns[3] = Vec4{t.x, t.y, t.z, 1.0f};
    return result;
}

constexpr Mat4 scaling(Vec3 s) {
    Mat4 result{};
    result.at(0, 0) = s.x;
    result.at(1, 1) = s.y;
    result.at(2, 2) = s.z;
    result.at(3, 3) = 1.0f;
    return result;
}

/// Rotation about an arbitrary axis, angle in radians, counter-clockwise when
/// looking down the axis towards the origin (right-hand rule).
inline Mat4 rotation(Vec3 axis, f32 radians) {
    const Vec3 a = normalize(axis);
    const f32 c = std::cos(radians);
    const f32 s = std::sin(radians);
    const f32 t = 1.0f - c;

    Mat4 result = identity();
    result.at(0, 0) = t * a.x * a.x + c;
    result.at(0, 1) = t * a.x * a.y - s * a.z;
    result.at(0, 2) = t * a.x * a.z + s * a.y;

    result.at(1, 0) = t * a.x * a.y + s * a.z;
    result.at(1, 1) = t * a.y * a.y + c;
    result.at(1, 2) = t * a.y * a.z - s * a.x;

    result.at(2, 0) = t * a.x * a.z - s * a.y;
    result.at(2, 1) = t * a.y * a.z + s * a.x;
    result.at(2, 2) = t * a.z * a.z + c;
    return result;
}

// --- inverse ----------------------------------------------------------------

/// General 4x4 inverse via cofactor expansion.
///
/// Returns the identity for a singular matrix instead of dividing by zero. In
/// practice most matrices here are affine and could use a much cheaper
/// specialisation, but a correct general version is the right thing to have
/// before optimising.
inline Mat4 inverse(const Mat4& mat) {
    // Flattened to a plain array first: the cofactor expansion is far easier to
    // read (and to check against a reference) with single-index addressing.
    f32 m[16]{};
    for (usize col = 0; col < 4; ++col) {
        for (usize row = 0; row < 4; ++row) {
            m[col * 4 + row] = mat.at(row, col);
        }
    }

    f32 inv[16]{};

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];

    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];

    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];

    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    const f32 det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f) {
        return identity();
    }

    const f32 invDet = 1.0f / det;
    Mat4 result{};
    for (usize col = 0; col < 4; ++col) {
        for (usize row = 0; row < 4; ++row) {
            result.at(row, col) = inv[col * 4 + row] * invDet;
        }
    }
    return result;
}

// --- camera matrices --------------------------------------------------------
//
// World space here is right-handed with +Y up, and the camera looks down its
// own -Z axis - the same convention OpenGL tutorials use.
//
// Vulkan's clip space is NOT the same as OpenGL's, in two ways that bite:
//   * +Y points DOWN on screen, so the projection flips Y;
//   * depth runs from 0 at the near plane to 1 at the far plane, not -1 to 1.
// Both corrections are baked into the matrices below, which is why passing a
// glm matrix built with default settings into a Vulkan renderer renders the
// scene upside down and with broken depth.

/// Right-handed perspective projection producing Vulkan clip space.
/// `fovYRadians` is the vertical field of view, `aspect` is width / height.
inline Mat4 perspective(f32 fovYRadians, f32 aspect, f32 nearPlane, f32 farPlane) {
    const f32 focal = 1.0f / std::tan(fovYRadians * 0.5f);

    Mat4 result{};
    result.at(0, 0) = focal / aspect;
    result.at(1, 1) = -focal; // negated: Vulkan's +Y is down
    result.at(2, 2) = farPlane / (nearPlane - farPlane);
    result.at(2, 3) = (farPlane * nearPlane) / (nearPlane - farPlane);
    result.at(3, 2) = -1.0f; // perspective divide takes w from -z
    return result;
}

/// Orthographic projection producing Vulkan clip space.
inline Mat4 orthographic(f32 left, f32 right, f32 bottom, f32 top, f32 nearPlane, f32 farPlane) {
    Mat4 result = identity();
    result.at(0, 0) = 2.0f / (right - left);
    result.at(1, 1) = -2.0f / (top - bottom); // negated for the same reason
    result.at(2, 2) = -1.0f / (farPlane - nearPlane);
    result.at(0, 3) = -(right + left) / (right - left);
    result.at(1, 3) = (top + bottom) / (top - bottom);
    result.at(2, 3) = -nearPlane / (farPlane - nearPlane);
    return result;
}

/// Builds a view matrix: the inverse of the camera's world transform.
inline Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
    const Vec3 forward = normalize(target - eye);
    const Vec3 right = normalize(cross(forward, up));
    const Vec3 trueUp = cross(right, forward);

    Mat4 result = identity();
    result.at(0, 0) = right.x;
    result.at(0, 1) = right.y;
    result.at(0, 2) = right.z;
    result.at(1, 0) = trueUp.x;
    result.at(1, 1) = trueUp.y;
    result.at(1, 2) = trueUp.z;
    result.at(2, 0) = -forward.x;
    result.at(2, 1) = -forward.y;
    result.at(2, 2) = -forward.z;
    result.at(0, 3) = -dot(right, eye);
    result.at(1, 3) = -dot(trueUp, eye);
    result.at(2, 3) = dot(forward, eye);
    return result;
}

} // namespace fumar
