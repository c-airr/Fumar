#include "fumar/render/material_gpu.hpp"

namespace fumar {

u32 textureSlot(TextureHandle handle, usize textureCount, u32 neutral) {
    if (!handle.valid() || handle.index >= textureCount || handle.index >= kMaxSceneTextures) {
        return neutral;
    }
    return handle.index;
}

MaterialRecord toRecord(const Material& material, const NeutralTextures& neutral, usize textureCount) {
    return MaterialRecord{
        .baseColorFactor = material.baseColorFactor,
        .emissive = material.emissiveColor * material.emissiveIntensity,
        .metallic = material.metallic,
        .roughness = material.roughness,
        .normalScale = material.normalScale,
        .occlusionStrength = material.occlusionStrength,
        .alphaCutoff = material.alphaMode == AlphaMode::Mask ? material.alphaCutoff : -1.0f,
        .uvScale = material.uvScale,
        .baseColorTexture = textureSlot(material.baseColor, textureCount, neutral.white),
        .metallicRoughnessTexture = textureSlot(material.metallicRoughness, textureCount, neutral.white),
        .normalTexture = textureSlot(material.normal, textureCount, neutral.flatNormal),
        .occlusionTexture = textureSlot(material.occlusion, textureCount, neutral.white),
        .emissiveTexture = textureSlot(material.emissive, textureCount, neutral.black),
        .padding = 0,
    };
}

} // namespace fumar
