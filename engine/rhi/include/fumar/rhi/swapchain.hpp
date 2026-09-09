#pragma once

#include "fumar/core/types.hpp"
#include "fumar/rhi/vk_common.hpp"

#include <optional>
#include <vector>

namespace fumar::rhi {

class Device;

/// The chain of images the GPU renders into and the compositor displays.
///
/// A swapchain is tied to the exact size of its surface, so resizing the window
/// invalidates it. Vulkan reports this through eErrorOutOfDateKHR rather than
/// resizing anything for you, and handling that is most of what this class does.
class Swapchain {
public:
    /// With `vsync` on, presentation is capped to the display refresh rate.
    /// Off, the renderer runs as fast as it can - useful for benchmarking, but
    /// on a trivial scene it means thousands of pointless frames per second
    /// heating the GPU for nothing.
    Swapchain(Device& device, vk::SurfaceKHR surface, Extent2D windowSize, bool vsync = true);
    ~Swapchain() = default;

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    /// Rebuilds for a new window size. Stalls the GPU first, since the images
    /// about to be destroyed may still be in use by frames in flight.
    ///
    /// Returns false when the surface currently has zero area - a minimised
    /// window - and leaves the existing swapchain untouched. The caller should
    /// skip the frame and try again later.
    bool recreate(Extent2D windowSize);

    /// The size the surface reports right now, which is what a new swapchain
    /// would actually get.
    ///
    /// This is NOT always the window size the OS reports: a minimised window on
    /// Windows still has a plausible size according to SDL, while the Vulkan
    /// surface says 0x0. Vulkan's answer is the one that matters here.
    vk::Extent2D surfaceExtent() const;

    vk::SwapchainKHR handle() const { return *m_swapchain; }

    /// Pixel format of the swapchain images. The render pipeline has to be
    /// created for this exact format.
    vk::Format format() const { return m_format; }

    vk::Extent2D extent() const { return m_extent; }

    u32 imageCount() const { return static_cast<u32>(m_images.size()); }

    vk::Image image(u32 index) const { return m_images[index]; }

    vk::ImageView imageView(u32 index) const { return *m_imageViews[index]; }

    /// Asks the presentation engine for the next image to draw into.
    ///
    /// The call returns as soon as an image is reserved, which is BEFORE it is
    /// actually free - the semaphore is signalled when it really is, so the
    /// first thing that writes to it must wait on that semaphore.
    ///
    /// Returns nothing when the swapchain no longer matches the surface, which
    /// is the caller's cue to recreate it and skip the frame.
    std::optional<u32> acquireNextImage(vk::Semaphore signalSemaphore);

    /// Queues an image for display. Returns false when the swapchain has gone
    /// stale and should be recreated.
    bool present(u32 imageIndex, vk::Semaphore waitSemaphore);

private:
    void create(Extent2D windowSize, vk::SwapchainKHR oldSwapchain);

    Device& m_device;
    vk::SurfaceKHR m_surface;

    vk::UniqueSwapchainKHR m_swapchain;

    // Swapchain images are owned by the presentation engine, so they are plain
    // handles - destroying them is not ours to do. The views we create on top
    // of them ARE ours, hence UniqueImageView.
    std::vector<vk::Image> m_images;
    std::vector<vk::UniqueImageView> m_imageViews;

    vk::Format m_format = vk::Format::eUndefined;
    vk::Extent2D m_extent{};
    bool m_vsync = true;
};

} // namespace fumar::rhi
