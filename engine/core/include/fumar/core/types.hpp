#pragma once

#include <cstddef>
#include <cstdint>

namespace fumar {

// Fixed-width aliases. Graphics code is full of sizes, offsets and bit masks
// that must match what the GPU expects exactly, and `unsigned long` says
// nothing about its width while `u32` does.

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using f32 = float;
using f64 = double;

using usize = std::size_t;
using isize = std::ptrdiff_t;

/// A size in pixels.
///
/// Lives in core so that modules which must not depend on Vulkan or SDL still
/// share a vocabulary type for window and image dimensions.
struct Extent2D {
    u32 width = 0;
    u32 height = 0;

    friend constexpr bool operator==(const Extent2D&, const Extent2D&) = default;
};

} // namespace fumar
