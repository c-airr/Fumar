#pragma once

#include "fumar/core/types.hpp"

#include <filesystem>
#include <optional>
#include <vector>

namespace fumar {

/// Reads a compiled SPIR-V module from disk.
///
/// The result is a vector of 32-bit words rather than bytes, because that is
/// what vkCreateShaderModule expects: SPIR-V is a stream of words, and the
/// pointer handed to Vulkan has to be 4-byte aligned. Reading into a
/// std::vector<u32> gets both the alignment and the size check for free.
///
/// Returns nothing if the file is missing, unreadable, or not a whole number
/// of words long.
std::optional<std::vector<u32>> readSpirvFile(const std::filesystem::path& path);

} // namespace fumar
