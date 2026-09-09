#include "fumar/rhi/buffer.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/core/log.hpp"
#include "fumar/rhi/device.hpp"

#include <vk_mem_alloc.h>

#include <cstring>
#include <utility>

namespace fumar::rhi {

Buffer::Buffer(Device& device, const BufferDesc& desc) : m_device(&device), m_size(desc.size) {
    FUMAR_ASSERT(desc.size > 0);

    const VkBufferCreateInfo bufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = desc.size,
        .usage = static_cast<VkBufferUsageFlags>(desc.usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };

    VmaAllocationCreateInfo allocationInfo{};
    // AUTO lets VMA pick the memory type from the usage flags and the access
    // pattern below, instead of us hard-coding a heap index that differs
    // between vendors.
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    if (desc.hostVisible) {
        // SEQUENTIAL_WRITE promises we only ever write forwards and never read
        // back, which lets VMA hand out write-combined memory. Reading from
        // that is catastrophically slow, so the promise matters.
        allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                               VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocationInfo allocationResult{};
    const VkResult result = vmaCreateBuffer(m_device->allocator(), &bufferInfo, &allocationInfo, &buffer,
                                            &m_allocation, &allocationResult);
    FUMAR_VERIFY_MSG(result == VK_SUCCESS, "vmaCreateBuffer failed: {}",
                     vk::to_string(static_cast<vk::Result>(result)));

    m_buffer = buffer;
    m_mapped = allocationResult.pMappedData;
}

Buffer::~Buffer() {
    destroy();
}

Buffer::Buffer(Buffer&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)),
      m_buffer(std::exchange(other.m_buffer, nullptr)),
      m_allocation(std::exchange(other.m_allocation, nullptr)),
      m_size(std::exchange(other.m_size, 0)),
      m_mapped(std::exchange(other.m_mapped, nullptr)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        destroy();
        m_device = std::exchange(other.m_device, nullptr);
        m_buffer = std::exchange(other.m_buffer, nullptr);
        m_allocation = std::exchange(other.m_allocation, nullptr);
        m_size = std::exchange(other.m_size, 0);
        m_mapped = std::exchange(other.m_mapped, nullptr);
    }
    return *this;
}

void Buffer::write(const void* data, usize bytes, usize offset) {
    FUMAR_VERIFY_MSG(m_mapped != nullptr, "write() on a buffer that is not host-visible");
    FUMAR_VERIFY_MSG(offset + bytes <= m_size, "write of {} bytes at offset {} overruns a {} byte buffer",
                     bytes, offset, m_size);

    std::memcpy(static_cast<u8*>(m_mapped) + offset, data, bytes);
    // No explicit flush: VMA_MEMORY_USAGE_AUTO with HOST_ACCESS picks coherent
    // memory whenever it is available, and vmaCreateBuffer would have failed
    // otherwise. Non-coherent memory would need vmaFlushAllocation here.
}

void Buffer::destroy() {
    if (m_device != nullptr && m_buffer) {
        vmaDestroyBuffer(m_device->allocator(), m_buffer, m_allocation);
        m_buffer = nullptr;
        m_allocation = nullptr;
        m_mapped = nullptr;
    }
}

} // namespace fumar::rhi
