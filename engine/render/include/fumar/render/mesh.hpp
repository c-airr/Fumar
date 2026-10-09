#pragma once

#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"
#include "fumar/geometry/editable_mesh.hpp"
#include "fumar/rhi/acceleration_structure.hpp"
#include "fumar/rhi/buffer.hpp"
#include "fumar/rhi/vk_common.hpp"

#include <array>
#include <span>
#include <vector>

namespace fumar {

namespace rhi {
class Device;
class UploadContext;
} // namespace rhi

/// One vertex, laid out exactly as the GPU will read it.
///
/// The struct is a plain aggregate of floats so it can be memcpy'd into a
/// vertex buffer. The attribute descriptions below tell Vulkan how to decode
/// those bytes, and their locations must match the `layout(location = N)`
/// declarations in mesh.vert - a mismatch is not a compile error, it is garbage
/// geometry.
struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;

    /// Describes the buffer itself: how many bytes one vertex occupies, and
    /// whether the index advances per vertex or per instance.
    static vk::VertexInputBindingDescription binding();

    /// Describes the fields inside that stride.
    static std::array<vk::VertexInputAttributeDescription, 3> attributes();
};

/// An axis-aligned bounding box in the mesh own local space.
///
/// Cheap to test a ray against and cheap to compute, which is what makes it the
/// standard first pass for picking: reject almost everything against boxes,
/// then do exact triangle tests only on what survives. fumar stops at the box,
/// which is accurate enough to click on an object and wrong only at the corners
/// of very non-boxy shapes.
struct Bounds {
    Vec3 min{0.0f, 0.0f, 0.0f};
    Vec3 max{0.0f, 0.0f, 0.0f};

    Vec3 centre() const { return (min + max) * 0.5f; }
    Vec3 extent() const { return max - min; }
};

/// Vertex and index buffers in device-local memory, plus the draw call.
///
/// Indices matter more than they look: a cube has 8 distinct corners but 36
/// index entries, so the vertex data is shared rather than duplicated 36 times.
/// The GPU also caches recently transformed vertices by index, so reuse is
/// faster as well as smaller.
class Mesh {
public:
    Mesh() = default;

    /// `polygonSizes` says how the triangles group into the faces they were
    /// cut from - see geometry::TriangleMesh. Empty when nobody knows, as for a
    /// mesh imported from another tool.
    Mesh(rhi::Device& device, rhi::UploadContext& upload, std::span<const Vertex> vertices,
         std::span<const u32> indices, std::span<const u32> polygonSizes = {});

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&&) noexcept = default;
    Mesh& operator=(Mesh&&) noexcept = default;

    /// Binds the buffers and issues the indexed draw.
    void draw(vk::CommandBuffer cmd) const;

    u32 indexCount() const { return m_indexCount; }

    /// Local-space bounds, computed once when the mesh was built.
    const Bounds& bounds() const { return m_bounds; }

    /// This mesh's triangles as the ray tracing hardware sees them, or an
    /// invalid structure when the GPU cannot trace rays.
    ///
    /// Built alongside the vertex and index buffers, from the same data. It
    /// describes the mesh in its OWN space, so one is enough however many times
    /// the mesh appears in the scene.
    const rhi::BottomLevelStructure& accelerationStructure() const { return m_blas; }

    /// Addresses of the vertex and index buffers in the GPU's address space.
    ///
    /// The rasteriser reaches these by having them BOUND; a ray that lands on a
    /// triangle has no such binding, because it did not know which mesh it was
    /// going to hit. Handing the shader the addresses is how it reads the
    /// geometry it found - the same buffers, no copy.
    const rhi::Buffer& vertexBuffer() const { return m_vertexBuffer; }
    const rhi::Buffer& indexBuffer() const { return m_indexBuffer; }

    /// The same vertices and indices the GPU has, kept on the CPU.
    ///
    /// The buffers above live in device-local memory, which the CPU cannot read
    /// back without a staging copy and a wait. Anything that needs the geometry
    /// on this side - opening a mesh in the Modeler, saving one to a file - reads
    /// these instead. Costs the size of the mesh in RAM, which for anything
    /// modelled by hand is nothing.
    std::span<const Vertex> vertices() const { return m_vertices; }
    std::span<const u32> indices() const { return m_indices; }
    std::span<const u32> polygonSizes() const { return m_polygonSizes; }

    /// The faces this mesh was made of, rebuilt from the copy above: exactly
    /// when the polygon sizes are known, by joining coplanar triangles when
    /// they are not.
    geometry::EditableMesh toEditable() const;

    bool valid() const { return m_indexCount > 0; }

private:
    rhi::Buffer m_vertexBuffer;
    rhi::Buffer m_indexBuffer;
    rhi::BottomLevelStructure m_blas;
    u32 m_indexCount = 0;
    Bounds m_bounds;

    std::vector<Vertex> m_vertices;
    std::vector<u32> m_indices;
    std::vector<u32> m_polygonSizes;
};

/// Uploads an editable mesh: triangulated, with a normal per corner as its
/// smoothing angle decides, and its polygon sizes remembered so the faces can
/// be recovered later.
///
/// Every mesh the engine generates goes through here - the cube, the sphere,
/// whatever the Modeler produces - so there is one way to turn faces into
/// buffers rather than one hand-written generator per shape. Returns an empty
/// mesh, which draws nothing, when there are no faces at all: a model whose
/// every face was deleted is a legitimate thing to have open.
Mesh makeMesh(rhi::Device& device, rhi::UploadContext& upload, const geometry::EditableMesh& mesh);

} // namespace fumar
