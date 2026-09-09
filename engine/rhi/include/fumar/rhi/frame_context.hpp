#pragma once

#include "fumar/core/types.hpp"
#include "fumar/rhi/vk_common.hpp"

#include <array>
#include <vector>

namespace fumar::rhi {

class Device;

/// Per-frame command buffers and synchronisation primitives.
///
/// The CPU records frame N+1 while the GPU executes frame N, so every resource
/// the CPU touches while recording needs kFramesInFlight copies - otherwise it
/// would be overwriting memory the GPU is still reading.
///
/// The presentation semaphores are the exception: there is one per SWAPCHAIN
/// IMAGE, not per frame in flight. vkQueuePresentKHR gives no way to know when
/// it is done waiting on a semaphore, so reusing one after only kFramesInFlight
/// frames can signal it while the presentation engine is still waiting on it.
/// Tutorials get this wrong constantly; the synchronisation validation layer
/// flags it immediately.
class FrameContext {
public:
    FrameContext(Device& device, u32 swapchainImageCount);

    FrameContext(const FrameContext&) = delete;
    FrameContext& operator=(const FrameContext&) = delete;

    /// Reallocates the per-image semaphores after the swapchain changed size.
    void onSwapchainRecreated(u32 swapchainImageCount);

    /// Blocks until the GPU has finished the frame that last used this slot.
    /// Without this the CPU would race ahead and overwrite a command buffer
    /// that is still executing.
    void waitForFrameSlot();

    /// Resets the fence for this slot. Deliberately separate from the wait:
    /// the fence must only be un-signalled once the frame is definitely going
    /// to be submitted, or an early return leaves it unsignalled forever and
    /// the next wait on it deadlocks.
    void resetFence();

    /// Resets and begins this slot's command buffer.
    vk::CommandBuffer beginCommandBuffer();

    vk::CommandBuffer commandBuffer() const { return m_frames[m_currentFrame].commandBuffer; }

    /// Signalled by the presentation engine when the acquired image is actually
    /// free to write to.
    vk::Semaphore imageAvailable() const { return *m_frames[m_currentFrame].imageAvailable; }

    /// Signalled when rendering into a given swapchain image is complete.
    vk::Semaphore renderFinished(u32 imageIndex) const { return *m_renderFinished[imageIndex]; }

    vk::Fence inFlightFence() const { return *m_frames[m_currentFrame].inFlight; }

    /// Moves to the next slot. Call once per presented frame.
    void advance() { m_currentFrame = (m_currentFrame + 1) % kFramesInFlight; }

    u32 currentFrame() const { return m_currentFrame; }

private:
    struct PerFrame {
        vk::UniqueCommandPool pool;
        /// Owned by the pool above, so it is a plain handle: destroying the
        /// pool frees every buffer allocated from it.
        vk::CommandBuffer commandBuffer;
        vk::UniqueSemaphore imageAvailable;
        vk::UniqueFence inFlight;
    };

    Device& m_device;
    std::array<PerFrame, kFramesInFlight> m_frames;
    std::vector<vk::UniqueSemaphore> m_renderFinished;
    u32 m_currentFrame = 0;
};

} // namespace fumar::rhi
