#include "fumar/render/mesh.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/rhi/device.hpp"
#include "fumar/rhi/upload_context.hpp"

#include <array>

namespace fumar {

vk::VertexInputBindingDescription Vertex::binding() {
    return vk::VertexInputBindingDescription{
        .binding = 0,
        .stride = sizeof(Vertex),
        // Advance one vertex at a time. eInstance would advance once per
        // instance instead, which is how per-instance transforms get fed in.
        .inputRate = vk::VertexInputRate::eVertex,
    };
}

std::array<vk::VertexInputAttributeDescription, 3> Vertex::attributes() {
    return {
        vk::VertexInputAttributeDescription{
            .location = 0,
            .binding = 0,
            // "SFloat" means signed float; R32G32B32 is three 32-bit channels.
            // The colour-channel naming is historical - this is a position.
            .format = vk::Format::eR32G32B32Sfloat,
            .offset = offsetof(Vertex, position),
        },
        vk::VertexInputAttributeDescription{
            .location = 1,
            .binding = 0,
            .format = vk::Format::eR32G32B32Sfloat,
            .offset = offsetof(Vertex, normal),
        },
        vk::VertexInputAttributeDescription{
            .location = 2,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = offsetof(Vertex, uv),
        },
    };
}

Mesh::Mesh(rhi::Device& device, rhi::UploadContext& upload, std::span<const Vertex> vertices,
           std::span<const u32> indices)
    : m_indexCount(static_cast<u32>(indices.size())) {
    FUMAR_ASSERT(!vertices.empty() && !indices.empty());

    m_vertexBuffer = upload.createDeviceBuffer(vertices.data(), vertices.size_bytes(),
                                               vk::BufferUsageFlagBits::eVertexBuffer);
    m_indexBuffer = upload.createDeviceBuffer(indices.data(), indices.size_bytes(),
                                              vk::BufferUsageFlagBits::eIndexBuffer);
    (void)device;
}

void Mesh::draw(vk::CommandBuffer cmd) const {
    if (!valid()) {
        return;
    }

    // Binding 0, matching Vertex::binding(). The offset is where in the buffer
    // this mesh's vertices start - always zero here, but a real engine packs
    // many meshes into one buffer and uses offsets to tell them apart.
    cmd.bindVertexBuffers(0, m_vertexBuffer.handle(), {0});
    cmd.bindIndexBuffer(m_indexBuffer.handle(), 0, vk::IndexType::eUint32);
    cmd.drawIndexed(m_indexCount, 1, 0, 0, 0);
}

Mesh makeCube(rhi::Device& device, rhi::UploadContext& upload) {
    // Six faces, four vertices each. Wound counter-clockwise when seen from
    // outside, which is the glTF convention fumar follows throughout.
    const std::array<Vertex, 24> vertices{{
        // +Z (front)
        {{-0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        {{0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{-0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        // -Z (back)
        {{0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},
        {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
        {{-0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}},
        {{0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}},
        // +X (right)
        {{0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},
        {{0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
        {{0.5f, 0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        // -X (left)
        {{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},
        {{-0.5f, -0.5f, 0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
        {{-0.5f, 0.5f, 0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{-0.5f, 0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        // +Y (top)
        {{-0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
        {{0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
        {{0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
        {{-0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        // -Y (bottom)
        {{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},
        {{0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
        {{0.5f, -0.5f, 0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}},
        {{-0.5f, -0.5f, 0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}},
    }};

    std::vector<u32> indices;
    indices.reserve(36);
    for (u32 face = 0; face < 6; ++face) {
        const u32 base = face * 4;
        // Two triangles per quad, sharing the diagonal 0-2.
        indices.insert(indices.end(), {base + 0, base + 1, base + 2, base + 2, base + 3, base + 0});
    }

    return Mesh(device, upload, vertices, indices);
}

Mesh makePlane(rhi::Device& device, rhi::UploadContext& upload, f32 halfSize, f32 uvTiling) {
    const std::array<Vertex, 4> vertices{{
        {{-halfSize, 0.0f, halfSize}, {0.0f, 1.0f, 0.0f}, {0.0f, uvTiling}},
        {{halfSize, 0.0f, halfSize}, {0.0f, 1.0f, 0.0f}, {uvTiling, uvTiling}},
        {{halfSize, 0.0f, -halfSize}, {0.0f, 1.0f, 0.0f}, {uvTiling, 0.0f}},
        {{-halfSize, 0.0f, -halfSize}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
    }};

    const std::array<u32, 6> indices{0, 1, 2, 2, 3, 0};

    return Mesh(device, upload, vertices, indices);
}

} // namespace fumar
