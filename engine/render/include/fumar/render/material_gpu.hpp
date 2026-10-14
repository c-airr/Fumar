#pragma once

#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"
#include "fumar/render/resources.hpp"

#include <cstddef>

namespace fumar {

/// The most textures the shaders reach: the size of the array every
/// material's maps are read from, on a GPU whose limits allow it. A GPU with
/// lower limits gets a smaller array - see Renderer's texture slots - and the
/// shaders are told its size through a specialization constant
/// (kMaxSceneTextures in shaders/material.glsl).
inline constexpr u32 kMaxSceneTextures = 1024;

/// Where the neutral textures sit in that array. A material slot with no map
/// of its own reads one of these, so a shader never has to ask.
///
/// White stands in for every map that multiplies a factor - colour,
/// metal-rough, occlusion, and emission too: glTF's emission is the colour
/// times the map, and with no map the colour alone must shine.
struct NeutralTextures {
    u32 white = 0;
    u32 flatNormal = 0;  ///< (0.5, 0.5, 1): a normal map that tilts nothing
};

/// One material as the shaders read it: shaders/material.glsl declares the
/// same struct, rasteriser and ray hits alike.
///
/// Laid out to std430's rules as well as C++'s - every vec3 followed by a
/// float, the vec2 on an eight-byte boundary, the size a multiple of sixteen -
/// so the GPUs without scalar block layout read it the same way. The offsets
/// are asserted below; the GLSL comments state the same numbers.
struct MaterialRecord {
    Vec4 baseColorFactor;
    Vec3 emissive;      ///< colour * intensity, premultiplied
    f32 metallic;
    f32 roughness;
    f32 normalScale;
    f32 occlusionStrength;
    f32 alphaCutoff;    ///< negative for an opaque material
    Vec2 uvScale;
    u32 baseColorTexture;
    u32 metallicRoughnessTexture;
    u32 normalTexture;
    u32 occlusionTexture;
    u32 emissiveTexture;
    u32 padding;
};

static_assert(sizeof(MaterialRecord) == 80, "shaders/material.glsl reads records with an 80-byte stride");
static_assert(offsetof(MaterialRecord, emissive) == 16);
static_assert(offsetof(MaterialRecord, metallic) == 28);
static_assert(offsetof(MaterialRecord, roughness) == 32);
static_assert(offsetof(MaterialRecord, alphaCutoff) == 44);
static_assert(offsetof(MaterialRecord, uvScale) == 48);
static_assert(offsetof(MaterialRecord, baseColorTexture) == 56);
static_assert(offsetof(MaterialRecord, emissiveTexture) == 72);

/// The array slot a texture is read from, or `neutral` when it cannot be read:
/// no texture, a handle past the `textureCount` the scene has, or a texture
/// past the end of an array of `slots`.
u32 textureSlot(TextureHandle handle, usize textureCount, u32 neutral, u32 slots);

/// A material as the shaders want it, for an array of `slots` textures.
MaterialRecord toRecord(const Material& material, const NeutralTextures& neutral, usize textureCount, u32 slots);

} // namespace fumar
