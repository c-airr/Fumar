#pragma once

#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"
#include "fumar/render/mesh.hpp"
#include "fumar/rhi/image.hpp"
#include "fumar/rhi/vk_common.hpp"
#include "fumar/scene/handles.hpp"

#include <string>
#include <vector>

namespace fumar {

/// What a surface looks like.
///
/// Only base colour for now. A physically based material would add metallic,
/// roughness, normal and occlusion maps, all of which slot into the same
/// descriptor set - the structure here is already the right shape for that.
struct Material {
    std::string name;
    TextureHandle baseColor;

    /// Multiplied with the texture. glTF uses it both for untextured materials
    /// and for tinting textured ones.
    Vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};

    /// Descriptor set 1, built when the material is registered. Cached here
    /// because building it per draw would be pure overhead - the contents never
    /// change after load.
    vk::DescriptorSet descriptorSet;
};

/// Owns the GPU resources that scene nodes name by handle.
///
/// The scene stores a MeshHandle; this is what turns it back into vertex
/// buffers. Splitting the two means the same mesh can be referenced by a
/// hundred nodes while existing once on the GPU, and it means the scene can be
/// serialised without touching Vulkan.
class ResourceRegistry {
public:
    ResourceRegistry() = default;

    ResourceRegistry(const ResourceRegistry&) = delete;
    ResourceRegistry& operator=(const ResourceRegistry&) = delete;

    MeshHandle addMesh(Mesh mesh);
    TextureHandle addTexture(rhi::Image texture);
    MaterialHandle addMaterial(Material material);

    const Mesh& mesh(MeshHandle handle) const;
    const rhi::Image& texture(TextureHandle handle) const;
    const Material& material(MaterialHandle handle) const;
    Material& material(MaterialHandle handle);

    bool has(MeshHandle handle) const { return handle.valid() && handle.index < m_meshes.size(); }
    bool has(TextureHandle handle) const { return handle.valid() && handle.index < m_textures.size(); }
    bool has(MaterialHandle handle) const { return handle.valid() && handle.index < m_materials.size(); }

    /// Used for nodes whose material is missing or invalid, so a broken asset
    /// renders as an obvious checkerboard rather than crashing.
    MaterialHandle fallbackMaterial() const { return m_fallbackMaterial; }
    void setFallbackMaterial(MaterialHandle handle) { m_fallbackMaterial = handle; }

    usize meshCount() const { return m_meshes.size(); }
    usize textureCount() const { return m_textures.size(); }
    usize materialCount() const { return m_materials.size(); }

    /// Releases everything. Must run while the device is still alive and idle.
    void clear();

private:
    std::vector<Mesh> m_meshes;
    std::vector<rhi::Image> m_textures;
    std::vector<Material> m_materials;
    MaterialHandle m_fallbackMaterial;
};

} // namespace fumar
