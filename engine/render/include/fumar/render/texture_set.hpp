#pragma once

#include "fumar/core/types.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace fumar {

/// Which file of a texture set is which map.
///
/// Texture sites hand out a material as a folder of images, one per map, named
/// by a suffix: ambientCG writes Bricks059_2K-JPG_Color.jpg, Poly Haven writes
/// brick_wall_diff_2k.jpg. This is the table of those suffixes; the files it
/// does not recognise - displacement, previews - are left out.
struct TextureSetFiles {
    std::filesystem::path baseColor;
    std::filesystem::path normal;
    std::filesystem::path roughness;
    std::filesystem::path metallic;
    std::filesystem::path occlusion;
    std::filesystem::path emissive;

    /// The normal map is DirectX-style, Y pointing down; flipped on load.
    bool normalIsDirectX = false;

    /// Enough to make a material of: a colour or a normal map.
    bool usable() const { return !baseColor.empty() || !normal.empty(); }
};

/// Sorts `files` into maps by name. Case does not matter; a suffix counts only
/// where it ends the name or is followed by '_', '-' or '.', so "_normal" does
/// not claim "_normalgl". Where several files fit one map, the suffix listed
/// first wins - the OpenGL normal map over the DirectX one.
TextureSetFiles findTextureSet(std::span<const std::filesystem::path> files);

/// The same, for the images in a folder.
TextureSetFiles findTextureSet(const std::filesystem::path& folder);

/// Eight-bit RGBA pixels, row by row from the top.
struct Pixels {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba;
};

/// Decodes an image file into RGBA, whatever its channel count or bit depth.
/// Nothing on failure.
std::optional<Pixels> loadPixels(const std::filesystem::path& file);

struct ImageSize {
    u32 width = 0;
    u32 height = 0;
};

/// An image's size from its header alone - before anything decodes it or
/// allocates for it, which is where a file too large for the GPU is turned
/// away. Nothing if the file is not an image this can read.
std::optional<ImageSize> readImageSize(const std::filesystem::path& file);

/// Separate roughness and metalness images as one texture in glTF's layout:
/// roughness in G, metalness in B, R and A at 255. Each is read from its R
/// channel. A missing one reads 255, for the material's factor to scale. The
/// result has the larger of the two sizes, the smaller resampled bilinearly;
/// with neither, it is one pixel.
Pixels packMetallicRoughness(const Pixels* roughness, const Pixels* metallic);

/// Writes PNG. False if the file could not be written.
bool writePng(const std::filesystem::path& file, const Pixels& pixels);

} // namespace fumar
