#pragma once

#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"
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

    bool valid() const { return m_indexCount > 0; }

private:
    rhi::Buffer m_vertexBuffer;
    rhi::Buffer m_indexBuffer;
    u32 m_indexCount = 0;
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

} // namespace fumar
