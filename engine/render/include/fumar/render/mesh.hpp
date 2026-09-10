#pragma once

#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"
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
    Mesh(rhi::Device& device, rhi::UploadContext& upload, std::span<const Vertex> vertices,
         std::span<const u32> indices);

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

    bool valid() const { return m_indexCount > 0; }

private:
    rhi::Buffer m_vertexBuffer;
    rhi::Buffer m_indexBuffer;
    rhi::BottomLevelStructure m_blas;
    u32 m_indexCount = 0;
    Bounds m_bounds;
};

/// A unit cube with correct per-face normals and UVs.
///
/// Built from 24 vertices rather than 8: a corner shared between three faces
/// needs a different normal for each of them, so the position is duplicated
/// once per face. Sharing all eight would give smooth-shaded, rounded-looking
/// corners instead of flat faces.
Mesh makeCube(rhi::Device& device, rhi::UploadContext& upload);

/// A flat ground plane of the given half-extent, lying in the XZ plane.
Mesh makePlane(rhi::Device& device, rhi::UploadContext& upload, f32 halfSize, f32 uvTiling = 1.0f);

/// A cylinder standing on the XZ plane, centred on the origin.
///
/// The side is built from `segments` quads whose normals point straight out
/// from the axis, so it shades smoothly; the caps are separate fans with flat
/// normals, because a cap and the side meeting at an edge need different
/// normals there - sharing them would round the rim over.
Mesh makeCylinder(rhi::Device& device, rhi::UploadContext& upload, f32 radius, f32 height,
                  u32 segments = 32);

} // namespace fumar
