#include "fumar/rhi/upload_context.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/rhi/device.hpp"
#include "fumar/rhi/image.hpp"

#include <limits>

namespace fumar::rhi {

UploadContext::UploadContext(Device& device) : m_device(device) {
    const vk::Device handle = device.handle();

    m_pool = handle.createCommandPoolUnique(vk::CommandPoolCreateInfo{
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = device.queueFamilies().graphics,
    });

    const auto buffers = handle.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
        .commandPool = *m_pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    });
    m_commandBuffer = buffers.front();

    // Unsignalled, unlike the per-frame fences: every submit here resets it
    // before use, and starting signalled would let the first wait return before
    // the work had run.
    m_fence = handle.createFenceUnique(vk::FenceCreateInfo{});
}

void UploadContext::immediateSubmit(const std::function<void(vk::CommandBuffer)>& record) {
    const vk::Device handle = m_device.handle();

    handle.resetFences(*m_fence);
    m_commandBuffer.reset();
    m_commandBuffer.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    });

    record(m_commandBuffer);

    m_commandBuffer.end();

    const vk::CommandBufferSubmitInfo commandBufferInfo{.commandBuffer = m_commandBuffer};
    const vk::SubmitInfo2 submitInfo{
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &commandBufferInfo,
    };

    m_device.graphicsQueue().submit2(submitInfo, *m_fence);

    const vk::Result result = handle.waitForFences(*m_fence, VK_TRUE, std::numeric_limits<u64>::max());
    FUMAR_VERIFY_MSG(result == vk::Result::eSuccess, "upload fence wait failed: {}", vk::to_string(result));
}

Buffer UploadContext::createDeviceBuffer(const void* data, usize bytes, vk::BufferUsageFlags usage) {
    FUMAR_ASSERT(data != nullptr && bytes > 0);

    Buffer staging(m_device, BufferDesc{
                                 .size = bytes,
                                 .usage = vk::BufferUsageFlagBits::eTransferSrc,
                                 .hostVisible = true,
                             });
    staging.write(data, bytes);

    Buffer result(m_device, BufferDesc{
                                .size = bytes,
                                // eTransferDst is what makes it a legal copy
                                // destination; omitting it is a validation error.
                                .usage = usage | vk::BufferUsageFlagBits::eTransferDst,
                                .hostVisible = false,
                            });

    immediateSubmit([&](vk::CommandBuffer cmd) {
        cmd.copyBuffer(staging.handle(), result.handle(), vk::BufferCopy{.size = bytes});
    });

    // staging is destroyed on the way out, which is safe because
    // immediateSubmit already waited for the copy to finish.
    return result;
}

void UploadContext::uploadImage(Image& image, const void* pixels, usize bytes) {
    FUMAR_ASSERT(pixels != nullptr && bytes > 0);

    Buffer staging(m_device, BufferDesc{
                                 .size = bytes,
                                 .usage = vk::BufferUsageFlagBits::eTransferSrc,
                                 .hostVisible = true,
                             });
    staging.write(pixels, bytes);

    const vk::ImageSubresourceRange wholeImage{
        .aspectMask = image.aspect(),
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };

    immediateSubmit([&](vk::CommandBuffer cmd) {
        // Undefined -> TransferDstOptimal. There is nothing to wait for, since
        // nothing has ever touched this image; eTopOfPipe as the source stage
        // states exactly that.
        const vk::ImageMemoryBarrier2 toTransferDst{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image.handle(),
            .subresourceRange = wholeImage,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &toTransferDst,
        });

        // Zero bufferRowLength and bufferImageHeight mean tightly packed rows,
        // which is what stb_image hands back.
        const vk::BufferImageCopy region{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {
                .aspectMask = image.aspect(),
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {image.extent().width, image.extent().height, 1},
        };
        cmd.copyBufferToImage(staging.handle(), image.handle(), vk::ImageLayout::eTransferDstOptimal, region);

        // TransferDstOptimal -> ShaderReadOnlyOptimal so the fragment shader can
        // sample it.
        const vk::ImageMemoryBarrier2 toShaderRead{
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image.handle(),
            .subresourceRange = wholeImage,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &toShaderRead,
        });
    });
}

} // namespace fumar::rhi
