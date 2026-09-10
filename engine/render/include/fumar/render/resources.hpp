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

/// Where a mesh came from, so it can be rebuilt when a scene is loaded.
///
/// A MeshHandle is a runtime index and means nothing in a file - reload in a
/// different order and it points at something else. What survives being written
/// to disk is a description of how to produce the mesh again, which is exactly
/// what this is.
struct MeshSource {
    /// "cube", "plane" or "cylinder" for generated meshes; empty when imported.
    std::string shape;

    /// Shape parameters. Their meaning depends on the shape: plane uses x for
    /// half-size and y for UV tiling, cylinder uses x, y and z for radius,
    /// height and segment count.
    Vec4 parameters{};

    /// Source file, for imported meshes.
    std::string file;

    /// Which primitive inside that file. The loader visits them in a fixed
    /// order, so the index is stable as long as the file is.
    u32 primitive = 0;

    bool imported() const { return !file.empty(); }
};

/// A generated mesh: a shape name and its parameters.
///
/// A small factory rather than a designated initialiser at each call site,
/// because naming only some fields warns and naming all of them means every
/// call has to be revisited when the struct grows.
inline MeshSource proceduralMesh(std::string shape, Vec4 parameters = {}) {
    MeshSource source;
    source.shape = std::move(shape);
    source.parameters = parameters;
    return source;
}

/// A mesh that came out of a file, identified by that file and its position in
/// it.
inline MeshSource importedMesh(std::string file, u32 primitive) {
    MeshSource source;
    source.file = std::move(file);
    source.primitive = primitive;
    return source;
}

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

    /// Texture file this material was built from, for saving. Empty for a flat
    /// colour, and for glTF materials - those record their origin below instead,
    /// because a glTF texture is usually embedded in the file rather than
    /// sitting beside it as an image.
    std::string baseColorPath;

    /// The glTF file and material index this came from, for saving. Empty for
    /// materials created in the editor.
    std::string sourceFile;
    u32 sourceIndex = 0;
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

    /// Registers a mesh together with a description of how to rebuild it.
    /// The description is what a saved scene stores in place of the handle.
    MeshHandle addMesh(Mesh mesh, MeshSource source = {});
    TextureHandle addTexture(rhi::Image texture);
    MaterialHandle addMaterial(Material material);

    const Mesh& mesh(MeshHandle handle) const;
    const MeshSource& meshSource(MeshHandle handle) const;
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

    // Parallel to m_meshes rather than a member of Mesh: a Mesh is GPU
    // resources, and where it came from is bookkeeping the GPU has no use for.
    std::vector<MeshSource> m_meshSources;
    std::vector<rhi::Image> m_textures;
    std::vector<Material> m_materials;
    MaterialHandle m_fallbackMaterial;
};

} // namespace fumar
