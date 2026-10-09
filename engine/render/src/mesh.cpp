#include "fumar/render/mesh.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/rhi/device.hpp"
#include "fumar/rhi/upload_context.hpp"

#include <array>
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
           std::span<const u32> indices, std::span<const u32> polygonSizes)
    : m_indexCount(static_cast<u32>(indices.size())),
      m_vertices(vertices.begin(), vertices.end()),
      m_indices(indices.begin(), indices.end()),
      m_polygonSizes(polygonSizes.begin(), polygonSizes.end()) {
    FUMAR_ASSERT(!vertices.empty() && !indices.empty());

    // Computed here, on the CPU copy, because once the data is in device-local
    // memory it cannot be read back without a staging copy.
    m_bounds.min = vertices[0].position;
    m_bounds.max = vertices[0].position;
    for (const Vertex& vertex : vertices) {
        m_bounds.min = min(m_bounds.min, vertex.position);
        m_bounds.max = max(m_bounds.max, vertex.position);
    }

    // The extra usage flags are only legal once the bufferDeviceAddress
    // feature is on, which Device only enables when it also enabled ray
    // tracing. Asking for them unconditionally would be a validation error on
    // every GPU that cannot trace.
    vk::BufferUsageFlags extra;
    if (device.rayTracingSupported()) {
        extra = vk::BufferUsageFlagBits::eShaderDeviceAddress |
                vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
    }

    m_vertexBuffer = upload.createDeviceBuffer(vertices.data(), vertices.size_bytes(),
                                               vk::BufferUsageFlagBits::eVertexBuffer | extra);
    m_indexBuffer = upload.createDeviceBuffer(indices.data(), indices.size_bytes(),
                                              vk::BufferUsageFlagBits::eIndexBuffer | extra);

    if (device.rayTracingSupported()) {
        m_blas = rhi::BottomLevelStructure(device, upload,
                                           rhi::BlasGeometry{
                                               .vertices = m_vertexBuffer.deviceAddress(),
                                               .indices = m_indexBuffer.deviceAddress(),
                                               .vertexCount = static_cast<u32>(vertices.size()),
                                               .indexCount = m_indexCount,
                                               .vertexStride = sizeof(Vertex),
                                           });
    }
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

geometry::EditableMesh Mesh::toEditable() const {
    std::vector<Vec3> positions;
    std::vector<Vec2> uvs;
    positions.reserve(m_vertices.size());
    uvs.reserve(m_vertices.size());
    for (const Vertex& vertex : m_vertices) {
        positions.push_back(vertex.position);
        uvs.push_back(vertex.uv);
    }
    return geometry::fromTriangles(positions, uvs, m_indices, m_polygonSizes);
}

Mesh makeMesh(rhi::Device& device, rhi::UploadContext& upload, const geometry::EditableMesh& mesh) {
    const geometry::TriangleMesh triangles = mesh.triangulate();
    if (triangles.indices.empty()) {
        return Mesh{};
    }

    // Repacked into the interleaved layout the vertex shader reads. The
    // geometry module keeps its arrays separate because it neither knows nor
    // cares how a GPU wants them; this is the one place that does.
    std::vector<Vertex> vertices(triangles.positions.size());
    for (usize i = 0; i < vertices.size(); ++i) {
        vertices[i] = Vertex{triangles.positions[i], triangles.normals[i], triangles.uvs[i]};
    }

    return Mesh(device, upload, vertices, triangles.indices, triangles.polygonSizes);
}

} // namespace fumar
