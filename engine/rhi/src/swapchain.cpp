#include "fumar/rhi/swapchain.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/core/log.hpp"
#include "fumar/rhi/device.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace fumar::rhi {
namespace {

/// Prefers an 8-bit BGRA surface with sRGB encoding.
///
/// The sRGB part matters: with an sRGB format the hardware converts from linear
/// to display space on write, so lighting maths can stay linear. Picking a UNORM
/// format instead and doing nothing else gives a noticeably washed-out image.
vk::SurfaceFormatKHR chooseSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& available) {
    for (const vk::SurfaceFormatKHR& format : available) {
        if (format.format == vk::Format::eB8G8R8A8Srgb &&
            format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
            return format;
        }
    }

    FUMAR_WARN("preferred surface format B8G8R8A8_SRGB unavailable, falling back to {}",
               vk::to_string(available.front().format));
    return available.front();
}

/// Picks how finished frames reach the screen.
///
/// eFifo is a true queue synchronised to the display refresh: it never tears
/// and never drops a frame, but the CPU blocks once the queue is full. It is
/// also the only mode the specification guarantees exists.
///
/// eMailbox keeps one pending frame and replaces it rather than queuing, so
/// rendering faster than the display refreshes costs latency instead of
/// stalling - still no tearing, but the extra frames are thrown away.
///
/// eImmediate presents as soon as the frame is done, tearing and all. Only
/// useful for measuring raw throughput.
vk::PresentModeKHR choosePresentMode(const std::vector<vk::PresentModeKHR>& available, bool vsync) {
    const auto supports = [&available](vk::PresentModeKHR mode) {
        return std::find(available.begin(), available.end(), mode) != available.end();
    };

    if (vsync) {
        return vk::PresentModeKHR::eFifo;
    }
    if (supports(vk::PresentModeKHR::eMailbox)) {
        return vk::PresentModeKHR::eMailbox;
    }
    if (supports(vk::PresentModeKHR::eImmediate)) {
        return vk::PresentModeKHR::eImmediate;
    }
    return vk::PresentModeKHR::eFifo;
}

/// Resolves the swapchain size.
///
/// Most platforms pin it to the surface's current size, signalled by a
/// currentExtent of 0xFFFFFFFF meaning "you choose". Wayland is the notable
/// case where the application really does pick, hence the clamp.
vk::Extent2D chooseExtent(const vk::SurfaceCapabilitiesKHR& capabilities, Extent2D windowSize) {
    if (capabilities.currentExtent.width != std::numeric_limits<u32>::max()) {
        return capabilities.currentExtent;
    }

    return vk::Extent2D{
        std::clamp(windowSize.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
        std::clamp(windowSize.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height),
    };
}

} // namespace

Swapchain::Swapchain(Device& device, vk::SurfaceKHR surface, Extent2D windowSize, bool vsync)
    : m_device(device), m_surface(surface), m_vsync(vsync) {
    create(windowSize, nullptr);
}

vk::Extent2D Swapchain::surfaceExtent() const {
    const auto capabilities = m_device.physicalDevice().getSurfaceCapabilitiesKHR(m_surface);
    return chooseExtent(capabilities, Extent2D{m_extent.width, m_extent.height});
}

bool Swapchain::recreate(Extent2D windowSize) {
    // Check before touching anything. Destroying the old swapchain first and
    // only then discovering the surface has no area leaves the object in a
    // broken state with no swapchain at all.
    const vk::Extent2D target = surfaceExtent();
    if (target.width == 0 || target.height == 0) {
        return false;
    }

    // Images from the old swapchain may still be referenced by commands the GPU
    // has not finished. Waiting is heavy-handed but correct, and a resize is
    // rare enough that the stall does not matter.
    m_device.waitIdle();

    // Passing the old swapchain lets the driver reuse its resources and keeps
    // presentation going during the handover. It is retired by the create call,
    // and the old handle is released when m_swapchain is overwritten below.
    vk::UniqueSwapchainKHR old = std::move(m_swapchain);
    m_imageViews.clear();
    m_images.clear();

    create(windowSize, old ? *old : vk::SwapchainKHR{});
    return true;
}

void Swapchain::create(Extent2D windowSize, vk::SwapchainKHR oldSwapchain) {
    const vk::PhysicalDevice gpu = m_device.physicalDevice();

    const auto capabilities = gpu.getSurfaceCapabilitiesKHR(m_surface);
    const auto formats = gpu.getSurfaceFormatsKHR(m_surface);
    const auto presentModes = gpu.getSurfacePresentModesKHR(m_surface);

    FUMAR_VERIFY_MSG(!formats.empty() && !presentModes.empty(), "surface reports no usable configuration");

    const vk::SurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(formats);
    const vk::PresentModeKHR presentMode = choosePresentMode(presentModes, m_vsync);
    const vk::Extent2D extent = chooseExtent(capabilities, windowSize);

    // Last line of defence. recreate() screens this case out beforehand; hitting
    // it here means the very first swapchain was requested for a window that was
    // already minimised.
    FUMAR_VERIFY_MSG(extent.width > 0 && extent.height > 0,
                     "cannot create a swapchain for a zero-sized surface (is the window minimised?)");

    // One more than the minimum, so the application always has an image to draw
    // into while the presentation engine holds one. maxImageCount == 0 means
    // "no limit".
    u32 imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    const QueueFamilies& families = m_device.queueFamilies();
    const std::array<u32, 2> familyIndices{families.graphics, families.present};

    // When one family both renders and presents, the images never change owner
    // and eExclusive is the faster mode. When they differ, eConcurrent lets both
    // families touch the image without explicit ownership transfers - simpler,
    // and this path is rare on desktop hardware.
    const bool concurrent = !families.sameFamily();

    const vk::SwapchainCreateInfoKHR createInfo{
        .surface = m_surface,
        .minImageCount = imageCount,
        .imageFormat = surfaceFormat.format,
        .imageColorSpace = surfaceFormat.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment,
        .imageSharingMode = concurrent ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive,
        .queueFamilyIndexCount = concurrent ? 2u : 0u,
        .pQueueFamilyIndices = concurrent ? familyIndices.data() : nullptr,
        // Some devices (phones, mostly) present rotated; echoing back the
        // current transform means "leave it as it is".
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque,
        .presentMode = presentMode,
        // Allows the driver to skip shading pixels hidden by another window.
        .clipped = VK_TRUE,
        .oldSwapchain = oldSwapchain,
    };

    m_swapchain = m_device.handle().createSwapchainKHRUnique(createInfo);
    m_format = surfaceFormat.format;
    m_extent = extent;

    m_images = m_device.handle().getSwapchainImagesKHR(*m_swapchain);

    m_imageViews.clear();
    m_imageViews.reserve(m_images.size());
    for (const vk::Image& image : m_images) {
        m_imageViews.push_back(m_device.handle().createImageViewUnique(vk::ImageViewCreateInfo{
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = m_format,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        }));
    }

    FUMAR_INFO("swapchain: {}x{}, {} images, {}, {}", m_extent.width, m_extent.height, m_images.size(),
               vk::to_string(m_format), vk::to_string(presentMode));
}

std::optional<u32> Swapchain::acquireNextImage(vk::Semaphore signalSemaphore) {
    // vulkan.hpp turns error results into exceptions. An out-of-date swapchain
    // is not really an error though - it is the normal way a resize is
    // reported - so it is caught here and turned back into a value.
    try {
        const auto result = m_device.handle().acquireNextImageKHR(
            *m_swapchain, std::numeric_limits<u64>::max(), signalSemaphore, nullptr);

        // eSuboptimal means the image still works but no longer matches the
        // surface ideally. Rendering this frame is fine; the resize path picks
        // it up afterwards.
        return result.value;
    } catch (const vk::OutOfDateKHRError&) {
        return std::nullopt;
    }
}

bool Swapchain::present(u32 imageIndex, vk::Semaphore waitSemaphore) {
    const vk::SwapchainKHR swapchain = *m_swapchain;

    const vk::PresentInfoKHR presentInfo{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &waitSemaphore,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &imageIndex,
    };

    try {
        const vk::Result result = m_device.presentQueue().presentKHR(presentInfo);
        return result != vk::Result::eSuboptimalKHR;
    } catch (const vk::OutOfDateKHRError&) {
        return false;
    }
}

} // namespace fumar::rhi
