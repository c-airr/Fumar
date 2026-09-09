#pragma once

#include "fumar/core/types.hpp"
#include "fumar/rhi/image.hpp"

#include <filesystem>

namespace fumar {

namespace rhi {
class Device;
class UploadContext;
} // namespace rhi

/// Decodes an image file (PNG, JPEG, ...) into a device-local texture.
///
/// `srgb` picks the format, and it is not a cosmetic choice. Colour textures
/// are authored in sRGB space, so an sRGB format makes the hardware linearise
/// them on every sample - which is what the lighting maths assumes. Data
/// textures (normal maps, roughness, metallic) hold numbers rather than colours
/// and must NOT be linearised, so they pass srgb = false.
///
/// Returns an invalid Image if the file is missing or cannot be decoded.
rhi::Image loadTextureFromFile(rhi::Device& device, rhi::UploadContext& upload,
                               const std::filesystem::path& path, bool srgb = true);

/// Same, for an image already in memory - glTF files can embed their textures
/// in a buffer rather than referencing separate files.
rhi::Image loadTextureFromMemory(rhi::Device& device, rhi::UploadContext& upload, const void* data,
                                 usize bytes, bool srgb = true);

} // namespace fumar
