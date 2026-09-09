#include "fumar/render/mesh.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/rhi/device.hpp"
#include "fumar/rhi/upload_context.hpp"

#include <array>
#include <cmath>
#include <vector>

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

    // Computed here, on the CPU copy, because once the data is in device-local
    // memory it cannot be read back without a staging copy.
    m_bounds.min = vertices[0].position;
    m_bounds.max = vertices[0].position;
    for (const Vertex& vertex : vertices) {
        m_bounds.min = min(m_bounds.min, vertex.position);
        m_bounds.max = max(m_bounds.max, vertex.position);
    }

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

Mesh makeCylinder(rhi::Device& device, rhi::UploadContext& upload, f32 radius, f32 height, u32 segments) {
    FUMAR_ASSERT(segments >= 3);

    std::vector<Vertex> vertices;
    std::vector<u32> indices;

    const f32 halfHeight = height * 0.5f;

    // --- side -------------------------------------------------------------
    // segments + 1 rings of vertices rather than segments: the seam needs two
    // vertices at the same position with U of 0 and 1, or the texture would be
    // squeezed backwards across the last quad.
    for (u32 i = 0; i <= segments; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(segments);
        const f32 angle = t * kTwoPi;
        const f32 x = std::cos(angle);
        const f32 z = std::sin(angle);

        // Points straight out from the axis, which is what makes the side
        // shade as a smooth curve rather than as flat facets.
        const Vec3 normal{x, 0.0f, z};

        vertices.push_back(Vertex{{x * radius, -halfHeight, z * radius}, normal, {t, 1.0f}});
        vertices.push_back(Vertex{{x * radius, halfHeight, z * radius}, normal, {t, 0.0f}});
    }

    for (u32 i = 0; i < segments; ++i) {
        const u32 base = i * 2;

        // Wound so the outward normal matches the winding. Checked rather than
        // guessed: for the first quad the edge cross product
        // (v3 - v0) x (v2 - v0) points along +X, which is where the surface
        // faces there. The other order points inward, and the cylinder then
        // renders as a black tube because only its inside survives culling and
        // the normals there face away from the light.
        indices.insert(indices.end(),
                       {base, base + 3, base + 2, base + 3, base, base + 1});
    }

    // --- caps -------------------------------------------------------------
    const auto addCap = [&](f32 y, f32 normalY) {
        const u32 centre = static_cast<u32>(vertices.size());
        vertices.push_back(Vertex{{0.0f, y, 0.0f}, {0.0f, normalY, 0.0f}, {0.5f, 0.5f}});

        for (u32 i = 0; i <= segments; ++i) {
            const f32 angle = static_cast<f32>(i) / static_cast<f32>(segments) * kTwoPi;
            const f32 x = std::cos(angle);
            const f32 z = std::sin(angle);
            vertices.push_back(Vertex{{x * radius, y, z * radius},
                                      {0.0f, normalY, 0.0f},
                                      {x * 0.5f + 0.5f, z * 0.5f + 0.5f}});
        }

        for (u32 i = 0; i < segments; ++i) {
            // The caps face opposite ways, so they wind opposite ways. Going
            // around the rim in increasing angle traces a clockwise path seen
            // from above, which is why the TOP cap is the one that needs its
            // triangles reversed.
            if (normalY > 0.0f) {
                indices.insert(indices.end(), {centre, centre + 2 + i, centre + 1 + i});
            } else {
                indices.insert(indices.end(), {centre, centre + 1 + i, centre + 2 + i});
            }
        }
    };

    addCap(halfHeight, 1.0f);
    addCap(-halfHeight, -1.0f);

    return Mesh(device, upload, vertices, indices);
}

} // namespace fumar
