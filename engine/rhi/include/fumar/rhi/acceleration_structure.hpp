#pragma once

#include "fumar/core/types.hpp"
#include "fumar/rhi/buffer.hpp"
#include "fumar/rhi/vk_common.hpp"

#include <span>

namespace fumar::rhi {

class Device;
class UploadContext;

/// One mesh's geometry, described by addresses rather than descriptors.
///
/// An acceleration structure build is not a shader: it is a fixed-function unit
/// that walks the vertex and index data itself, so it is handed raw GPU
/// addresses. Those come from Buffer::deviceAddress(), which is why the buffers
/// have to be created with eShaderDeviceAddress in the first place.
struct BlasGeometry {
    vk::DeviceAddress vertices = 0;
    vk::DeviceAddress indices = 0;
    u32 vertexCount = 0;
    u32 indexCount = 0;
    vk::DeviceSize vertexStride = 0;
};

/// A bottom-level acceleration structure: one mesh's triangles, in its own
/// object space, arranged into the tree the ray tracing hardware walks.
///
/// Built once, when the mesh is uploaded, and never touched again - which is
/// exactly why the hierarchy is split in two. Object space means a mesh used in
/// fifty places is only built once; where those fifty copies ARE is the top
/// level's business, and that is the part rebuilt every frame.
class BottomLevelStructure {
public:
    BottomLevelStructure() = default;
    BottomLevelStructure(Device& device, UploadContext& upload, const BlasGeometry& geometry);
    ~BottomLevelStructure();

    BottomLevelStructure(const BottomLevelStructure&) = delete;
    BottomLevelStructure& operator=(const BottomLevelStructure&) = delete;
    BottomLevelStructure(BottomLevelStructure&& other) noexcept;
    BottomLevelStructure& operator=(BottomLevelStructure&& other) noexcept;

    /// Address of the built structure, which is what a top-level instance
    /// points at. Zero for an empty structure.
    vk::DeviceAddress deviceAddress() const { return m_address; }

    bool valid() const { return static_cast<bool>(m_structure); }

private:
    void destroy();

    Device* m_device = nullptr;

    /// The structure object and the memory holding it are separate: Vulkan
    /// makes you allocate the buffer, then create the structure inside it.
    Buffer m_storage;
    vk::AccelerationStructureKHR m_structure;
    vk::DeviceAddress m_address = 0;
};

/// The top-level structure: where every instance of every mesh is this frame.
///
/// Rebuilt inside the frame's own command buffer rather than through a blocking
/// submit, because it changes whenever anything moves - which, in an editor
/// with a gizmo in your hand, is most frames. The buffers are allocated once
/// for a maximum instance count and reused; growing past it reallocates, which
/// stalls, so the initial capacity is generous.
///
/// One of these per frame in flight. The GPU may still be tracing against the
/// previous frame's copy while this frame's is being written.
class TopLevelStructure {
public:
    TopLevelStructure() = default;
    ~TopLevelStructure();

    TopLevelStructure(const TopLevelStructure&) = delete;
    TopLevelStructure& operator=(const TopLevelStructure&) = delete;
    TopLevelStructure(TopLevelStructure&& other) noexcept;
    TopLevelStructure& operator=(TopLevelStructure&& other) noexcept;

    /// Records a full rebuild into `cmd`, plus the barrier that makes the
    /// result readable by the fragment shaders that follow.
    ///
    /// Reallocates when `instances` no longer fits, which is why it needs the
    /// device. Safe to call with an empty span - the structure is then valid
    /// but hits nothing, which is what an empty scene should trace like.
    void record(Device& device, vk::CommandBuffer cmd,
                std::span<const vk::AccelerationStructureInstanceKHR> instances);

    vk::AccelerationStructureKHR handle() const { return m_structure; }

    bool valid() const { return static_cast<bool>(m_structure); }

private:
    void destroy();
    void reserve(Device& device, u32 instanceCapacity);

    Device* m_device = nullptr;

    /// Written by the CPU every frame, read by the build. Host-visible on
    /// purpose: a staging copy would need its own submit and barrier for data
    /// that is a few dozen bytes per object.
    Buffer m_instances;

    /// Working memory the build needs and nothing else ever reads. Its size is
    /// reported by the driver, not chosen by us.
    Buffer m_scratch;

    Buffer m_storage;
    vk::AccelerationStructureKHR m_structure;
    u32 m_capacity = 0;
};

/// Fills in a VkAccelerationStructureInstanceKHR's 3x4 transform from a Mat4.
///
/// Two conversions in one: the bottom row is dropped (an instance transform is
/// always affine), and the layout changes from column-major to ROW-major, which
/// is the one place in the API where that is true. Get it wrong and objects end
/// up somewhere plausible but wrong, which is far harder to spot than a crash.
vk::TransformMatrixKHR toTransformMatrix(const f32* columnMajor4x4);

} // namespace fumar::rhi
