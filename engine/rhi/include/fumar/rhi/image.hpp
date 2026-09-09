#pragma once

#include "fumar/core/types.hpp"
#include "fumar/rhi/vk_common.hpp"

VK_DEFINE_HANDLE(VmaAllocation)

namespace fumar::rhi {

class Device;

struct ImageDesc {
    vk::Extent2D extent;
    vk::Format format = vk::Format::eUndefined;
    vk::ImageUsageFlags usage;

    /// Which part of the image a view addresses: colour data, or the depth
    /// half of a depth/stencil format. Getting this wrong is a validation
    /// error, not a rendering artefact.
    vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor;

    u32 mipLevels = 1;
};

/// A device-local image plus its memory and a full-resource view.
///
/// Images differ from buffers in that the driver is free to rearrange their
/// memory layout - tiling, swizzling, compression - to suit how the hardware
/// samples them. That is why an image has a *layout* that must be transitioned
/// before each kind of use, while a buffer never does.
class Image {
public:
    Image() = default;
    Image(Device& device, const ImageDesc& desc);
    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;

    vk::Image handle() const { return m_image; }

    vk::ImageView view() const { return *m_view; }

    vk::Format format() const { return m_format; }

    vk::Extent2D extent() const { return m_extent; }

    vk::ImageAspectFlags aspect() const { return m_aspect; }

    bool valid() const { return static_cast<bool>(m_image); }

private:
    void destroy();

    Device* m_device = nullptr;
    vk::Image m_image;
    vk::UniqueImageView m_view;
    VmaAllocation m_allocation = nullptr;
    vk::Format m_format = vk::Format::eUndefined;
    vk::Extent2D m_extent;
    vk::ImageAspectFlags m_aspect;
};

} // namespace fumar::rhi
