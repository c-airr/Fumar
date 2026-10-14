// Materials, as the rasteriser and the rays both read them.
//
// One list of records and one array of textures for the whole scene, in the
// frame's descriptor set. A draw says which record is its material by index; a
// ray finds out what it hit and reads the same record by the same index. That
// is the point of the arrangement: a surface seen in a reflection is the
// surface the camera sees, not an approximation kept for the rays.

#ifndef FUMAR_MATERIAL_GLSL
#define FUMAR_MATERIAL_GLSL

/// Must match kMaxSceneTextures in engine/render/include/fumar/render/
/// material_gpu.hpp.
const int kMaxSceneTextures = 1024;

/// One material. Laid out exactly as fumar::MaterialRecord in
/// engine/render/include/fumar/render/material_gpu.hpp, whose offsets are
/// static_assert'ed there - 80 bytes, std430-clean so GPUs without scalar block
/// layout read it the same way.
struct MaterialRecord {
    vec4 baseColorFactor;            // offset 0
    vec3 emissive;                   // 16: colour * intensity
    float metallic;                  // 28
    float roughness;                 // 32
    float normalScale;               // 36
    float occlusionStrength;         // 40
    float alphaCutoff;               // 44: negative = opaque
    vec2 uvScale;                    // 48
    uint baseColorTexture;           // 56
    uint metallicRoughnessTexture;   // 60
    uint normalTexture;              // 64
    uint occlusionTexture;           // 68
    uint emissiveTexture;            // 72
    uint padding;                    // 76
};

/// Every texture in the scene, reachable by index.
///
/// A ray finds out what it hit only after it has hit it, so the only way to
/// read that surface's texture is for all of them to be reachable at once; the
/// rasteriser now reads its maps the same way. Slots past the end of the scene
/// hold a white texture, so nothing ever reads a descriptor nobody wrote.
layout(set = 0, binding = 3) uniform sampler2D sceneTextures[kMaxSceneTextures];

layout(set = 0, binding = 4, std430) readonly buffer MaterialBuffer {
    MaterialRecord materials[];
};

#endif // FUMAR_MATERIAL_GLSL
