#include "fumar/rhi/frame_context.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/core/log.hpp"
#include "fumar/rhi/device.hpp"

#include <limits>

namespace fumar::rhi {

FrameContext::FrameContext(Device& device, u32 swapchainImageCount) : m_device(device) {
    const vk::Device handle = device.handle();

    for (PerFrame& frame : m_frames) {
        // One pool per frame slot rather than one shared pool: resetting the
        // whole pool at once is cheaper than resetting individual buffers, and
        // pools are not thread-safe, so this also leaves room for recording
        // frames on different threads later.
        frame.pool = handle.createCommandPoolUnique(vk::CommandPoolCreateInfo{
            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = device.queueFamilies().graphics,
        });

        const auto buffers = handle.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
            .commandPool = *frame.pool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        });
        frame.commandBuffer = buffers.front();

        frame.imageAvailable = handle.createSemaphoreUnique(vk::SemaphoreCreateInfo{});

        // Created already signalled, so the very first frame does not wait on a
        // fence that nothing will ever signal.
        frame.inFlight = handle.createFenceUnique(vk::FenceCreateInfo{
            .flags = vk::FenceCreateFlagBits::eSignaled,
        });
    }

    onSwapchainRecreated(swapchainImageCount);

    FUMAR_INFO("frame context ready: {} frames in flight, {} presentation semaphores", kFramesInFlight,
               swapchainImageCount);
}

void FrameContext::onSwapchainRecreated(u32 swapchainImageCount) {
    m_renderFinished.clear();
    m_renderFinished.reserve(swapchainImageCount);
    for (u32 i = 0; i < swapchainImageCount; ++i) {
        m_renderFinished.push_back(m_device.handle().createSemaphoreUnique(vk::SemaphoreCreateInfo{}));
    }
}

void FrameContext::waitForFrameSlot() {
    const vk::Fence fence = inFlightFence();
    const vk::Result result =
        m_device.handle().waitForFences(fence, VK_TRUE, std::numeric_limits<u64>::max());
    FUMAR_VERIFY_MSG(result == vk::Result::eSuccess, "waiting for the frame fence failed: {}",
                     vk::to_string(result));
}

void FrameContext::resetFence() {
    m_device.handle().resetFences(inFlightFence());
}

vk::CommandBuffer FrameContext::beginCommandBuffer() {
    const vk::CommandBuffer cmd = commandBuffer();
    cmd.reset();
    cmd.begin(vk::CommandBufferBeginInfo{
        // Re-recorded from scratch every frame, so the driver may discard
        // anything it cached about the previous contents.
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    });
    return cmd;
}

} // namespace fumar::rhi
