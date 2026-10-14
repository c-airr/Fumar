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
    /// What the mesh is called where a person has to pick it: the Details
    /// panel's mesh list, the name of the file it is saved to.
    std::string name;

    /// "cube", "plane", "cylinder" or "sphere" for generated meshes; empty when
    /// imported or edited.
    std::string shape;

    /// Shape parameters. Their meaning depends on the shape: plane uses x for
    /// half-size and y for UV tiling, cylinder uses x, y and z for radius,
    /// height and segment count, sphere x, y and z for radius, segments and
    /// rings.
    Vec4 parameters{};

    /// Shaped by hand in the Modeler, so there is no recipe to rebuild it from
    /// - only the geometry itself. Saving a scene writes such a mesh to a file
    /// of its own, after which it is an imported mesh like any other.
    bool edited = false;

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
    source.name = shape;
    source.shape = std::move(shape);
    source.parameters = parameters;
    return source;
}

/// A mesh made in the Modeler.
inline MeshSource editedMesh(std::string name) {
    MeshSource source;
    source.name = std::move(name);
    source.edited = true;
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

/// Whether a surface can have holes cut in it by its colour's alpha.
///
/// No blending: real transparency needs sorting and a pass of its own, and is
/// drawn as opaque until it has one. A cutout - leaves, a chain-link fence -
/// is a discard in the shader and needs neither.
enum class AlphaMode : u8 {
    Opaque,
    Mask,
};

/// Where a material came from, which is how a saved scene finds it again.
struct MaterialOrigin {
    enum class Kind : u8 {
        /// Made in code or by importing a single image: saved as that image's
        /// path (in `file`, possibly empty) and the colour.
        Builtin,
        /// Material `index` of the glTF file `file`. Its textures are usually
        /// inside the file, so the file is what is recorded, not the images.
        Gltf,
        /// The .fmat file `file`.
        Fmat,
    };
    Kind kind = Kind::Builtin;
    std::string file;
    u32 index = 0;
};

/// What a surface looks like: physically based, metallic-roughness, the model
/// glTF uses.
///
/// Every map is optional. A slot with no texture reads a neutral one the
/// renderer keeps for the purpose - white for everything a factor multiplies,
/// a flat normal for the normal map - so the shaders never branch on whether
/// a map is there, and the factors below are what such a slot shows.
struct Material {
    std::string name;

    TextureHandle baseColor;          ///< sRGB
    TextureHandle metallicRoughness;  ///< linear; G = roughness, B = metallic
    TextureHandle normal;             ///< linear; tangent space, +Y up (OpenGL)
    TextureHandle occlusion;          ///< linear; R
    TextureHandle emissive;           ///< sRGB

    /// Multiplied with the texture. glTF uses it both for untextured materials
    /// and for tinting textured ones.
    Vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};

    /// 0 = dielectric, 1 = metal. There is nothing physical in between: the
    /// range exists so a texture that covers both kinds of surface has
    /// somewhere to interpolate, not so you can make half-metal.
    ///
    /// The two behave completely differently. A dielectric reflects about 4% of
    /// the light regardless of its colour and lets the rest through to be
    /// coloured by what is underneath - which is why plastic has white
    /// highlights whatever colour it is. A metal reflects nearly everything and
    /// tints it, and has no diffuse colour at all. Multiplies the map's B.
    f32 metallic = 0.0f;

    /// 0 = mirror, 1 = fully diffuse. The single control that most changes how
    /// a surface reads, because it decides how wide the highlight is.
    /// Multiplies the map's G.
    f32 roughness = 0.75f;

    /// How far the normal map tilts the surface: 0 is flat, 1 as authored.
    f32 normalScale = 1.0f;

    /// How much of the occlusion map is applied: 0 none, 1 all of it.
    f32 occlusionStrength = 1.0f;

    /// Light the surface gives off, as colour and a multiplier for it - the
    /// split KHR_materials_emissive_strength makes, so a colour can stay a
    /// colour and the brightness go past 1.
    Vec3 emissiveColor{0.0f, 0.0f, 0.0f};
    f32 emissiveIntensity = 1.0f;

    AlphaMode alphaMode = AlphaMode::Opaque;
    f32 alphaCutoff = 0.5f;

    /// How many times the texture repeats across the mesh's own 0..1 range.
    ///
    /// Needed because a node's SCALE does not touch its texture coordinates. A
    /// cube stretched into a ten-metre wall still has coordinates running 0 to
    /// 1 across it, so the bricks stretch with it and come out as smears. This
    /// is what puts the density back, and it belongs to the material rather
    /// than the mesh because the same wall mesh at two sizes wants two
    /// different answers.
    Vec2 uvScale{1.0f, 1.0f};

    MaterialOrigin origin;

    // --- where the maps came from, for writing the material back out --------
    // Absolute paths, empty where there is no file: a glTF's maps are inside
    // the glTF. Roughness and metalness may be two files, packed into one
    // texture on load, or one file already packed.
    std::string baseColorPath;
    std::string metallicPath;
    std::string roughnessPath;
    std::string packedMetallicRoughnessPath;
    std::string normalPath;
    bool normalIsDirectX = false;
    std::string occlusionPath;
    std::string emissivePath;
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

    /// Swaps in new geometry under an existing handle, and hands back the old.
    ///
    /// Hands it back rather than destroying it because the GPU may still be
    /// drawing the frame before this one with those buffers, and tracing rays
    /// against its acceleration structure. The caller keeps it alive until
    /// that frame is done - see Renderer::updateMesh.
    [[nodiscard]] Mesh replaceMesh(MeshHandle handle, Mesh mesh);

    void setMeshSource(MeshHandle handle, MeshSource source);
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
