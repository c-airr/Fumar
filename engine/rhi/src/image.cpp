#include "fumar/rhi/image.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/rhi/device.hpp"

#include <vk_mem_alloc.h>

#include <utility>

namespace fumar::rhi {

Image::Image(Device& device, const ImageDesc& desc)
    : m_device(&device), m_format(desc.format), m_extent(desc.extent), m_aspect(desc.aspect) {
    FUMAR_ASSERT(desc.extent.width > 0 && desc.extent.height > 0);

    const VkImageCreateInfo imageInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = static_cast<VkFormat>(desc.format),
        .extent = {desc.extent.width, desc.extent.height, 1},
        .mipLevels = desc.mipLevels,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        // OPTIMAL lets the driver choose whatever internal layout the hardware
        // samples fastest. LINEAR would be row-major and CPU-readable, but is
        // only supported for a narrow set of formats and is much slower.
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = static_cast<VkImageUsageFlags>(desc.usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        // Every image starts undefined and must be transitioned before use.
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    // Render targets and sampled textures are read by the GPU constantly, so
    // they are worth a dedicated fast allocation rather than a shared block.
    allocationInfo.priority = 1.0f;

    VkImage image = VK_NULL_HANDLE;
    const VkResult result =
        vmaCreateImage(m_device->allocator(), &imageInfo, &allocationInfo, &image, &m_allocation, nullptr);
    FUMAR_VERIFY_MSG(result == VK_SUCCESS, "vmaCreateImage failed: {}",
                     vk::to_string(static_cast<vk::Result>(result)));
    m_image = image;

    m_view = m_device->handle().createImageViewUnique(vk::ImageViewCreateInfo{
        .image = m_image,
        .viewType = vk::ImageViewType::e2D,
        .format = m_format,
        .subresourceRange = {
            .aspectMask = m_aspect,
            .baseMipLevel = 0,
            .levelCount = desc.mipLevels,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    });
}

Image::~Image() {
    destroy();
}

Image::Image(Image&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)),
      m_image(std::exchange(other.m_image, nullptr)),
      m_view(std::move(other.m_view)),
      m_allocation(std::exchange(other.m_allocation, nullptr)),
      m_format(other.m_format),
      m_extent(other.m_extent),
      m_aspect(other.m_aspect) {}

Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        destroy();
        m_device = std::exchange(other.m_device, nullptr);
        m_image = std::exchange(other.m_image, nullptr);
        m_view = std::move(other.m_view);
        m_allocation = std::exchange(other.m_allocation, nullptr);
        m_format = other.m_format;
        m_extent = other.m_extent;
        m_aspect = other.m_aspect;
    }
    return *this;
}

void Image::destroy() {
    // The view must go first: it references the image it was created from.
    m_view.reset();
    if (m_device != nullptr && m_image) {
        vmaDestroyImage(m_device->allocator(), m_image, m_allocation);
        m_image = nullptr;
        m_allocation = nullptr;
    }
}

} // namespace fumar::rhi
