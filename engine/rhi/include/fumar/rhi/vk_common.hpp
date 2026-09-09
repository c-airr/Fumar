#pragma once

// The configuration macros vulkan.hpp reacts to are set on the fumar::vulkan
// CMake target (see cmake/FumarVulkan.cmake), not here - defining them in a
// header would only work for files that include this one first.
#include <vulkan/vulkan.hpp>

#include "fumar/core/types.hpp"

namespace fumar::rhi {

/// How many frames the CPU is allowed to run ahead of the GPU.
///
/// Two is the usual choice: while the GPU works on frame N the CPU records
/// frame N+1, so neither waits on the other, and only two copies of the
/// per-frame resources are needed. Going higher adds latency for very little
/// throughput.
inline constexpr u32 kFramesInFlight = 2;

} // namespace fumar::rhi
