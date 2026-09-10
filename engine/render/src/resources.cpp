#include "fumar/render/resources.hpp"

#include "fumar/core/assert.hpp"

#include <utility>

namespace fumar {

MeshHandle ResourceRegistry::addMesh(Mesh mesh, MeshSource source) {
    const u32 index = static_cast<u32>(m_meshes.size());
    m_meshes.push_back(std::move(mesh));
    m_meshSources.push_back(std::move(source));
    return MeshHandle{index};
}

TextureHandle ResourceRegistry::addTexture(rhi::Image texture) {
    const u32 index = static_cast<u32>(m_textures.size());
    m_textures.push_back(std::move(texture));
    return TextureHandle{index};
}

MaterialHandle ResourceRegistry::addMaterial(Material material) {
    const u32 index = static_cast<u32>(m_materials.size());
    m_materials.push_back(std::move(material));
    return MaterialHandle{index};
}

const Mesh& ResourceRegistry::mesh(MeshHandle handle) const {
    FUMAR_ASSERT_MSG(has(handle), "mesh handle {} is not registered", handle.index);
    return m_meshes[handle.index];
}

const MeshSource& ResourceRegistry::meshSource(MeshHandle handle) const {
    FUMAR_ASSERT_MSG(has(handle), "mesh handle {} is not registered", handle.index);
    return m_meshSources[handle.index];
}

const rhi::Image& ResourceRegistry::texture(TextureHandle handle) const {
    FUMAR_ASSERT_MSG(has(handle), "texture handle {} is not registered", handle.index);
    return m_textures[handle.index];
}

const Material& ResourceRegistry::material(MaterialHandle handle) const {
    FUMAR_ASSERT_MSG(has(handle), "material handle {} is not registered", handle.index);
    return m_materials[handle.index];
}

Material& ResourceRegistry::material(MaterialHandle handle) {
    FUMAR_ASSERT_MSG(has(handle), "material handle {} is not registered", handle.index);
    return m_materials[handle.index];
}

void ResourceRegistry::clear() {
    // Meshes and textures own Vulkan objects, so the order matters only in that
    // all of it has to happen before the device goes away. Descriptor sets are
    // owned by the pool and released when it is destroyed or reset.
    m_meshes.clear();
    m_meshSources.clear();
    m_textures.clear();
    m_materials.clear();
    m_fallbackMaterial = MaterialHandle{};
}

} // namespace fumar
