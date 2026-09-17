#include "fumar/rhi/upload_context.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/rhi/device.hpp"
#include "fumar/rhi/image.hpp"

#include <algorithm>
#include <array>
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

    // Level 0 only: the smaller levels are not filled by the copy, and
    // generateMipmaps moves them itself as it walks down the chain.
    const vk::ImageSubresourceRange topLevel{
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
            // The WHOLE image here, unlike everything after it: every level
            // starts out undefined and has to leave that layout once.
            .subresourceRange =
                vk::ImageSubresourceRange{
                    .aspectMask = image.aspect(),
                    .baseMipLevel = 0,
                    .levelCount = image.mipLevels(),
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
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

        if (image.mipLevels() > 1) {
            generateMipmaps(cmd, image);
            return;
        }

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
            .subresourceRange = topLevel,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &toShaderRead,
        });
    });
}

void UploadContext::generateMipmaps(vk::CommandBuffer cmd, Image& image) {
    // Why a texture needs these at all.
    //
    // A surface in the distance covers few pixels, so one pixel of the screen
    // covers many texels of the texture. Sampling once picks ONE of them, more
    // or less arbitrarily, and the answer changes completely when the camera
    // moves a fraction of a degree - which is why an unfiltered ground plane
    // does not merely look noisy, it BOILS. Averaging the texels per pixel is
    // the right answer and far too expensive; a mip chain is that average,
    // computed once at load and looked up in constant time.
    //
    // Each level is the one above it halved with a linear filter, which is a
    // 2x2 box average. Not the best filter for this - a wider one is gentler
    // still - but the hardware does it for free during the blit.
    auto barrier = [&](u32 level, vk::ImageLayout oldLayout, vk::ImageLayout newLayout,
                       vk::AccessFlags2 srcAccess, vk::AccessFlags2 dstAccess) {
        const vk::ImageMemoryBarrier2 transition{
            .srcStageMask = vk::PipelineStageFlagBits2::eBlit,
            .srcAccessMask = srcAccess,
            .dstStageMask = vk::PipelineStageFlagBits2::eBlit,
            .dstAccessMask = dstAccess,
            .oldLayout = oldLayout,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image.handle(),
            .subresourceRange =
                vk::ImageSubresourceRange{
                    .aspectMask = image.aspect(),
                    .baseMipLevel = level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &transition,
        });
    };

    i32 width = static_cast<i32>(image.extent().width);
    i32 height = static_cast<i32>(image.extent().height);

    for (u32 level = 1; level < image.mipLevels(); ++level) {
        // The level being read from has to stop being a copy destination and
        // become a copy source. This is per level rather than per image because
        // the levels below have not been written yet.
        barrier(level - 1, vk::ImageLayout::eTransferDstOptimal,
                vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferWrite,
                vk::AccessFlagBits2::eTransferRead);

        // Never below one: a 1024x512 texture reaches height 1 while it still
        // has width to give, and an extent of zero is invalid.
        const i32 nextWidth = std::max(width / 2, 1);
        const i32 nextHeight = std::max(height / 2, 1);

        const vk::ImageBlit2 region{
            .srcSubresource =
                vk::ImageSubresourceLayers{
                    .aspectMask = image.aspect(),
                    .mipLevel = level - 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            .srcOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0},
                                                      vk::Offset3D{width, height, 1}},
            .dstSubresource =
                vk::ImageSubresourceLayers{
                    .aspectMask = image.aspect(),
                    .mipLevel = level,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            .dstOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0},
                                                      vk::Offset3D{nextWidth, nextHeight, 1}},
        };

        cmd.blitImage2(vk::BlitImageInfo2{
            .srcImage = image.handle(),
            .srcImageLayout = vk::ImageLayout::eTransferSrcOptimal,
            .dstImage = image.handle(),
            .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
            .regionCount = 1,
            .pRegions = &region,
            // Linear, so the halving averages rather than picking one of the
            // four texels - which would defeat the whole point.
            .filter = vk::Filter::eLinear,
        });

        width = nextWidth;
        height = nextHeight;
    }

    // Everything down to the second-to-last level is a transfer source by now;
    // the last one was never read from and is still a destination. Two ranges,
    // one barrier.
    const std::array<vk::ImageMemoryBarrier2, 2> toShaderRead{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eBlit,
            .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image.handle(),
            .subresourceRange =
                vk::ImageSubresourceRange{
                    .aspectMask = image.aspect(),
                    .baseMipLevel = 0,
                    .levelCount = image.mipLevels() - 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
        },
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eBlit,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image.handle(),
            .subresourceRange =
                vk::ImageSubresourceRange{
                    .aspectMask = image.aspect(),
                    .baseMipLevel = image.mipLevels() - 1,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
        },
    };

    cmd.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(toShaderRead.size()),
        .pImageMemoryBarriers = toShaderRead.data(),
    });
}

} // namespace fumar::rhi
