#include "fumar/rhi/acceleration_structure.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/core/log.hpp"
#include "fumar/rhi/device.hpp"
#include "fumar/rhi/upload_context.hpp"

#include <utility>

namespace fumar::rhi {
namespace {

/// Usage flags every buffer an acceleration structure lives in needs.
constexpr vk::BufferUsageFlags kStorageUsage =
    vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR |
    vk::BufferUsageFlagBits::eShaderDeviceAddress;

constexpr vk::BufferUsageFlags kScratchUsage =
    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

constexpr vk::BufferUsageFlags kInstanceUsage =
    vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR |
    vk::BufferUsageFlagBits::eShaderDeviceAddress;

/// Scratch memory has an alignment requirement the driver reports separately
/// from the size. VMA has no idea about it, so it is rounded up by hand.
vk::DeviceSize scratchAlignment(Device& device) {
    const auto chain = device.physicalDevice()
                           .getProperties2<vk::PhysicalDeviceProperties2,
                                           vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();
    return chain.get<vk::PhysicalDeviceAccelerationStructurePropertiesKHR>()
        .minAccelerationStructureScratchOffsetAlignment;
}

vk::DeviceSize alignUp(vk::DeviceSize value, vk::DeviceSize alignment) {
    return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
}

} // namespace

vk::TransformMatrixKHR toTransformMatrix(const f32* columnMajor4x4) {
    vk::TransformMatrixKHR result{};
    for (u32 row = 0; row < 3; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            // column * 4 + row reads the source as column-major; writing to
            // [row][column] stores it row-major.
            result.matrix[row][column] = columnMajor4x4[column * 4 + row];
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
//  bottom level
// ---------------------------------------------------------------------------

BottomLevelStructure::BottomLevelStructure(Device& device, UploadContext& upload,
                                           const BlasGeometry& geometry)
    : m_device(&device) {
    FUMAR_VERIFY_MSG(geometry.indexCount % 3 == 0, "a triangle mesh needs a multiple of 3 indices");
    const u32 triangleCount = geometry.indexCount / 3;
    if (triangleCount == 0) {
        return;
    }

    // --- describe the geometry ----------------------------------------------
    const vk::AccelerationStructureGeometryKHR geometryInfo{
        .geometryType = vk::GeometryTypeKHR::eTriangles,
        .geometry =
            vk::AccelerationStructureGeometryDataKHR{
                .triangles =
                    vk::AccelerationStructureGeometryTrianglesDataKHR{
                        // Only the position is read. The rest of the vertex is
                        // skipped over using the stride, which is why interleaved
                        // vertex data needs no repacking.
                        .vertexFormat = vk::Format::eR32G32B32Sfloat,
                        .vertexData = {.deviceAddress = geometry.vertices},
                        .vertexStride = geometry.vertexStride,
                        .maxVertex = geometry.vertexCount - 1,
                        .indexType = vk::IndexType::eUint32,
                        .indexData = {.deviceAddress = geometry.indices},
                    },
            },
        // eOpaque promises no any-hit shader wants to reject a hit, which lets
        // the hardware stop at the first triangle it finds. Transparency would
        // have to give this up.
        .flags = vk::GeometryFlagBitsKHR::eOpaque,
    };

    vk::AccelerationStructureBuildGeometryInfoKHR buildInfo{
        .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
        // Trace performance over build time. A mesh is built once and traced
        // against for the rest of the session, so this is the easy call.
        .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
        .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
        .geometryCount = 1,
        .pGeometries = &geometryInfo,
    };

    // --- ask how much memory it needs ---------------------------------------
    // There is no formula for this: how large the tree comes out is up to the
    // driver's own heuristics, so it is asked before anything is allocated.
    const vk::AccelerationStructureBuildSizesInfoKHR sizes =
        device.handle().getAccelerationStructureBuildSizesKHR(
            vk::AccelerationStructureBuildTypeKHR::eDevice, buildInfo, triangleCount);

    m_storage = Buffer(device, BufferDesc{
                                   .size = sizes.accelerationStructureSize,
                                   .usage = kStorageUsage,
                               });

    m_structure = device.handle().createAccelerationStructureKHR(
        vk::AccelerationStructureCreateInfoKHR{
            .buffer = m_storage.handle(),
            .size = sizes.accelerationStructureSize,
            .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
        });

    // Scratch is needed only while the build runs, so it is a local that dies
    // at the end of this function - after immediateSubmit has waited for it.
    const Buffer scratch(device, BufferDesc{
                                     .size = sizes.buildScratchSize + scratchAlignment(device),
                                     .usage = kScratchUsage,
                                 });

    buildInfo.dstAccelerationStructure = m_structure;
    buildInfo.scratchData.deviceAddress =
        alignUp(scratch.deviceAddress(), scratchAlignment(device));

    const vk::AccelerationStructureBuildRangeInfoKHR range{
        .primitiveCount = triangleCount,
    };

    upload.immediateSubmit([&](vk::CommandBuffer cmd) {
        const vk::AccelerationStructureBuildRangeInfoKHR* rangePtr = &range;
        cmd.buildAccelerationStructuresKHR(1, &buildInfo, &rangePtr);
    });

    m_address = device.handle().getAccelerationStructureAddressKHR(
        vk::AccelerationStructureDeviceAddressInfoKHR{.accelerationStructure = m_structure});
}

BottomLevelStructure::~BottomLevelStructure() {
    destroy();
}

BottomLevelStructure::BottomLevelStructure(BottomLevelStructure&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)),
      m_storage(std::move(other.m_storage)),
      m_structure(std::exchange(other.m_structure, nullptr)),
      m_address(std::exchange(other.m_address, 0)) {}

BottomLevelStructure& BottomLevelStructure::operator=(BottomLevelStructure&& other) noexcept {
    if (this != &other) {
        destroy();
        m_device = std::exchange(other.m_device, nullptr);
        m_storage = std::move(other.m_storage);
        m_structure = std::exchange(other.m_structure, nullptr);
        m_address = std::exchange(other.m_address, 0);
    }
    return *this;
}

void BottomLevelStructure::destroy() {
    if (m_device != nullptr && m_structure) {
        m_device->handle().destroyAccelerationStructureKHR(m_structure);
    }
    m_structure = nullptr;
    m_storage = Buffer{};
    m_address = 0;
}

// ---------------------------------------------------------------------------
//  top level
// ---------------------------------------------------------------------------

TopLevelStructure::~TopLevelStructure() {
    destroy();
}

TopLevelStructure::TopLevelStructure(TopLevelStructure&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)),
      m_instances(std::move(other.m_instances)),
      m_scratch(std::move(other.m_scratch)),
      m_storage(std::move(other.m_storage)),
      m_structure(std::exchange(other.m_structure, nullptr)),
      m_capacity(std::exchange(other.m_capacity, 0)) {}

TopLevelStructure& TopLevelStructure::operator=(TopLevelStructure&& other) noexcept {
    if (this != &other) {
        destroy();
        m_device = std::exchange(other.m_device, nullptr);
        m_instances = std::move(other.m_instances);
        m_scratch = std::move(other.m_scratch);
        m_storage = std::move(other.m_storage);
        m_structure = std::exchange(other.m_structure, nullptr);
        m_capacity = std::exchange(other.m_capacity, 0);
    }
    return *this;
}

void TopLevelStructure::destroy() {
    if (m_device != nullptr && m_structure) {
        m_device->handle().destroyAccelerationStructureKHR(m_structure);
    }
    m_structure = nullptr;
    m_instances = Buffer{};
    m_scratch = Buffer{};
    m_storage = Buffer{};
    m_capacity = 0;
}

void TopLevelStructure::reserve(Device& device, u32 instanceCapacity) {
    // Anything holding the old structure may still be in flight. The caller is
    // responsible for that; growing is rare enough that it stalls there.
    destroy();
    m_device = &device;

    // Rounded up in generous steps, so a scene that grows one object at a time
    // does not reallocate on every single one.
    m_capacity = std::max(64u, (instanceCapacity + 63u) / 64u * 64u);

    m_instances = Buffer(device,
                         BufferDesc{
                             .size = sizeof(vk::AccelerationStructureInstanceKHR) * m_capacity,
                             .usage = kInstanceUsage,
                             .hostVisible = true,
                         });

    const vk::AccelerationStructureGeometryKHR geometryInfo{
        .geometryType = vk::GeometryTypeKHR::eInstances,
        .geometry =
            vk::AccelerationStructureGeometryDataKHR{
                .instances =
                    vk::AccelerationStructureGeometryInstancesDataKHR{
                        .data = {.deviceAddress = m_instances.deviceAddress()},
                    },
            },
        .flags = vk::GeometryFlagBitsKHR::eOpaque,
    };

    const vk::AccelerationStructureBuildGeometryInfoKHR buildInfo{
        .type = vk::AccelerationStructureTypeKHR::eTopLevel,
        // Fast BUILD rather than fast trace, the opposite of the bottom level:
        // this one is rebuilt from scratch every frame, so the build cost is
        // paid sixty times a second and the trace saving never adds up.
        .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild,
        .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
        .geometryCount = 1,
        .pGeometries = &geometryInfo,
    };

    const vk::AccelerationStructureBuildSizesInfoKHR sizes =
        device.handle().getAccelerationStructureBuildSizesKHR(
            vk::AccelerationStructureBuildTypeKHR::eDevice, buildInfo, m_capacity);

    m_storage = Buffer(device, BufferDesc{
                                   .size = sizes.accelerationStructureSize,
                                   .usage = kStorageUsage,
                               });

    m_scratch = Buffer(device, BufferDesc{
                                   .size = sizes.buildScratchSize + scratchAlignment(device),
                                   .usage = kScratchUsage,
                               });

    m_structure = device.handle().createAccelerationStructureKHR(
        vk::AccelerationStructureCreateInfoKHR{
            .buffer = m_storage.handle(),
            .size = sizes.accelerationStructureSize,
            .type = vk::AccelerationStructureTypeKHR::eTopLevel,
        });

    FUMAR_DEBUG("top level structure sized for {} instances", m_capacity);
}

void TopLevelStructure::record(Device& device, vk::CommandBuffer cmd,
                               std::span<const vk::AccelerationStructureInstanceKHR> instances) {
    if (m_structure == nullptr || instances.size() > m_capacity) {
        // Only legitimate because the renderer waits for the device before the
        // first frame and whenever the scene is replaced.
        device.waitIdle();
        reserve(device, static_cast<u32>(instances.size()));
    }

    if (!instances.empty()) {
        m_instances.write(instances.data(),
                          instances.size() * sizeof(vk::AccelerationStructureInstanceKHR));
    }

    const vk::AccelerationStructureGeometryKHR geometryInfo{
        .geometryType = vk::GeometryTypeKHR::eInstances,
        .geometry =
            vk::AccelerationStructureGeometryDataKHR{
                .instances =
                    vk::AccelerationStructureGeometryInstancesDataKHR{
                        .data = {.deviceAddress = m_instances.deviceAddress()},
                    },
            },
        .flags = vk::GeometryFlagBitsKHR::eOpaque,
    };

    const vk::AccelerationStructureBuildGeometryInfoKHR buildInfo{
        .type = vk::AccelerationStructureTypeKHR::eTopLevel,
        .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild,
        .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
        .dstAccelerationStructure = m_structure,
        .geometryCount = 1,
        .pGeometries = &geometryInfo,
        .scratchData = {.deviceAddress = alignUp(m_scratch.deviceAddress(),
                                                 scratchAlignment(device))},
    };

    const vk::AccelerationStructureBuildRangeInfoKHR range{
        .primitiveCount = static_cast<u32>(instances.size()),
    };
    const vk::AccelerationStructureBuildRangeInfoKHR* rangePtr = &range;

    // The CPU wrote the instance buffer through a mapped pointer; the build
    // reads it on the GPU. Coherent memory makes the write visible, but the
    // dependency still has to be stated or the build may run first.
    const vk::MemoryBarrier2 hostWrite{
        .srcStageMask = vk::PipelineStageFlagBits2::eHost,
        .srcAccessMask = vk::AccessFlagBits2::eHostWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
        .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &hostWrite,
    });

    cmd.buildAccelerationStructuresKHR(1, &buildInfo, &rangePtr);

    // And the fragment shaders that trace against it must wait for the build.
    // Without this the first frame after a move traces the old tree, or worse,
    // one half-written.
    const vk::MemoryBarrier2 buildDone{
        .srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
        .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &buildDone,
    });
}

} // namespace fumar::rhi
