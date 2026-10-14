#include "fumar/render/material_gpu.hpp"

namespace fumar {

u32 textureSlot(TextureHandle handle, usize textureCount, u32 neutral, u32 slots) {
    if (!handle.valid() || handle.index >= textureCount || handle.index >= slots) {
        return neutral;
    }
    return handle.index;
}

MaterialRecord toRecord(const Material& material, const NeutralTextures& neutral, usize textureCount, u32 slots) {
    return MaterialRecord{
        .baseColorFactor = material.baseColorFactor,
        .emissive = material.emissiveColor * material.emissiveIntensity,
        .metallic = material.metallic,
        .roughness = material.roughness,
        .normalScale = material.normalScale,
        .occlusionStrength = material.occlusionStrength,
        .alphaCutoff = material.alphaMode == AlphaMode::Mask ? material.alphaCutoff : -1.0f,
        .uvScale = material.uvScale,
        .baseColorTexture = textureSlot(material.baseColor, textureCount, neutral.white, slots),
        .metallicRoughnessTexture = textureSlot(material.metallicRoughness, textureCount, neutral.white, slots),
        .normalTexture = textureSlot(material.normal, textureCount, neutral.flatNormal, slots),
        .occlusionTexture = textureSlot(material.occlusion, textureCount, neutral.white, slots),
        .emissiveTexture = textureSlot(material.emissive, textureCount, neutral.white, slots),
        .padding = 0,
    };
}

} // namespace fumar
