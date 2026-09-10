#pragma once

#include "fumar/core/types.hpp"
#include "fumar/rhi/vk_common.hpp"

// Opaque VMA handle, declared here so this header does not pull in the whole
// of vk_mem_alloc.h. Repeating VMA's own typedef is legal as long as it matches.
VK_DEFINE_HANDLE(VmaAllocation)

namespace fumar::rhi {

class Device;

struct BufferDesc {
    vk::DeviceSize size = 0;
    vk::BufferUsageFlags usage;

    /// Where the memory lives.
    ///
    /// Host-visible memory can be written by the CPU with a plain memcpy, but
    /// the GPU reads it across the PCIe bus. Device-local memory sits in VRAM
    /// and is far faster for the GPU, but the CPU cannot touch it - it has to
    /// be filled through a staging copy (see UploadContext).
    ///
    /// Rule of thumb: data written every frame (uniforms) goes host-visible,
    /// data uploaded once and read many times (meshes, textures) goes
    /// device-local.
    bool hostVisible = false;
};

/// A Vulkan buffer together with the memory backing it.
///
/// In raw Vulkan these are two separate objects that have to be created,
/// matched by memory type, and bound by hand. VMA collapses that into one call
/// and, just as importantly, sub-allocates from large blocks instead of asking
/// the driver for a separate allocation per buffer - drivers cap those at a few
/// thousand.
class Buffer {
public:
    Buffer() = default;
    Buffer(Device& device, const BufferDesc& desc);
    ~Buffer();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    vk::Buffer handle() const { return m_buffer; }

    vk::DeviceSize size() const { return m_size; }

    /// Persistent pointer into a host-visible buffer, or null for device-local
    /// memory. Mapping once and keeping the pointer is cheaper than map/unmap
    /// around every write, and Vulkan explicitly allows it.
    void* mapped() const { return m_mapped; }

    /// memcpy into a host-visible buffer. Asserts on device-local memory.
    void write(const void* data, usize bytes, usize offset = 0);

    /// The buffer's address in the GPU's own address space.
    ///
    /// Descriptors are the usual way a shader reaches a buffer, but an
    /// acceleration structure build is not a shader - it is a fixed-function
    /// unit handed raw addresses for the vertices, indices and scratch space.
    /// Requires the buffer to have been created with eShaderDeviceAddress.
    vk::DeviceAddress deviceAddress() const;

    bool valid() const { return static_cast<bool>(m_buffer); }

private:
    void destroy();

    Device* m_device = nullptr;
    vk::Buffer m_buffer;
    VmaAllocation m_allocation = nullptr;
    vk::DeviceSize m_size = 0;
    void* m_mapped = nullptr;
};

} // namespace fumar::rhi
