#pragma once

#include "fumar/core/types.hpp"
#include "fumar/rhi/vk_common.hpp"

#include <span>
#include <vector>

// VmaAllocator is an opaque pointer. Declaring the handle here rather than
// including vk_mem_alloc.h keeps a 20k-line header out of everything that
// merely holds a Device. Repeating the typedef is legal C++ as long as it is
// identical, which it is - this is the same macro VMA itself uses.
VK_DEFINE_HANDLE(VmaAllocator)

namespace fumar::rhi {

class Instance;

inline constexpr u32 kInvalidQueueFamily = ~0u;

/// Indices of the queue families the engine needs.
///
/// A queue family is a group of queues with identical capabilities. Graphics
/// and presentation are usually the same family, but the specification does not
/// guarantee it, so they are tracked separately.
struct QueueFamilies {
    u32 graphics = kInvalidQueueFamily;
    u32 present = kInvalidQueueFamily;

    bool complete() const { return graphics != kInvalidQueueFamily && present != kInvalidQueueFamily; }

    bool sameFamily() const { return graphics == present; }
};

/// A physical GPU plus the logical device opened on it.
///
/// Picks the most capable GPU that can present to the given surface, opens a
/// logical device with the features fumar requires, and brings up the memory
/// allocator.
class Device {
public:
    Device(const Instance& instance, vk::SurfaceKHR surface);
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;

    vk::PhysicalDevice physicalDevice() const { return m_physicalDevice; }

    vk::Device handle() const { return *m_device; }

    const QueueFamilies& queueFamilies() const { return m_queueFamilies; }

    vk::Queue graphicsQueue() const { return m_graphicsQueue; }

    vk::Queue presentQueue() const { return m_presentQueue; }

    VmaAllocator allocator() const { return m_allocator; }

    const vk::PhysicalDeviceProperties& properties() const { return m_properties; }

    /// Returns the first candidate format the GPU supports with the requested
    /// features, or eUndefined if none qualify.
    ///
    /// Needed because Vulkan guarantees almost nothing about which formats
    /// exist. Depth is the usual case: D32_SFLOAT is near-universal but not
    /// required, so the choice has to be made at runtime rather than assumed.
    vk::Format findSupportedFormat(std::span<const vk::Format> candidates, vk::ImageTiling tiling,
                                   vk::FormatFeatureFlags features) const;

    /// Blocks until the GPU has finished all outstanding work.
    ///
    /// A very blunt instrument - it stalls the whole pipeline. Only legitimate
    /// during shutdown and swapchain recreation; calling it every frame throws
    /// away all the parallelism the frames-in-flight machinery buys.
    void waitIdle() const;

private:
    vk::PhysicalDevice m_physicalDevice;
    vk::PhysicalDeviceProperties m_properties;
    QueueFamilies m_queueFamilies;

    vk::UniqueDevice m_device;
    vk::Queue m_graphicsQueue;
    vk::Queue m_presentQueue;

    // A raw handle, because VMA is a C library with no RAII wrapper. Destroyed
    // explicitly in the destructor, which is why this class is neither copyable
    // nor movable - a stale copy would double-free it.
    VmaAllocator m_allocator = nullptr;
};

} // namespace fumar::rhi
