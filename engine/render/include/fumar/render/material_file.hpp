#pragma once

#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"
#include "fumar/render/resources.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace fumar {

/// A material as a file describes it: factors and image paths, no GPU handles.
///
/// What a .fmat holds, what a texture-set folder becomes, and what an edited
/// material is written back out as. Paths are absolute here; the file stores
/// them relative to itself, so a folder of materials can move as a whole.
struct MaterialDesc {
    std::string name;

    std::filesystem::path baseColor;
    /// Either one image in glTF's layout (G roughness, B metalness) ...
    std::filesystem::path packedMetallicRoughness;
    /// ... or one or both of these, packed into that layout on load.
    std::filesystem::path metallic;
    std::filesystem::path roughness;
    std::filesystem::path normal;
    bool normalIsDirectX = false;
    std::filesystem::path occlusion;
    std::filesystem::path emissive;

    Vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    f32 metallicFactor = 0.0f;
    f32 roughnessFactor = 1.0f;
    f32 normalScale = 1.0f;
    f32 occlusionStrength = 1.0f;
    Vec3 emissiveColor{0.0f, 0.0f, 0.0f};
    f32 emissiveIntensity = 1.0f;
    AlphaMode alphaMode = AlphaMode::Opaque;
    f32 alphaCutoff = 0.5f;
    Vec2 uvScale{1.0f, 1.0f};

    friend bool operator==(const MaterialDesc&, const MaterialDesc&) = default;
};

/// Reads a .fmat. Nothing - with the reason logged - for a file that is
/// missing, is not JSON, is not a fumar material, is a newer format version,
/// or has a field of the wrong type. Never throws: a broken file is a warning,
/// not a crash.
std::optional<MaterialDesc> readMaterialFile(const std::filesystem::path& file);

/// Writes a .fmat, to a file beside the target first and then moved over it,
/// so a failed write leaves the previous one intact. False on failure.
bool writeMaterialFile(const std::filesystem::path& file, const MaterialDesc& desc);

/// The description a material would be saved as.
MaterialDesc describeMaterial(const Material& material);

} // namespace fumar
