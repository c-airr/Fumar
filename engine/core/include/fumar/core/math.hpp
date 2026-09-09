#pragma once

// Convenience header pulling in the whole maths module. Individual headers can
// still be included directly when only vectors or only matrices are needed.

#include "fumar/core/math/mat.hpp"
#include "fumar/core/math/quat.hpp"
#include "fumar/core/math/vec.hpp"
#include "fumar/core/types.hpp"

namespace fumar {

inline constexpr f32 kPi = 3.14159265358979323846f;
inline constexpr f32 kTwoPi = 2.0f * kPi;
inline constexpr f32 kHalfPi = 0.5f * kPi;

constexpr f32 radians(f32 degrees) { return degrees * (kPi / 180.0f); }

constexpr f32 degrees(f32 radians) { return radians * (180.0f / kPi); }

constexpr f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

constexpr f32 clamp(f32 value, f32 low, f32 high) {
    return value < low ? low : (value > high ? high : value);
}

} // namespace fumar
