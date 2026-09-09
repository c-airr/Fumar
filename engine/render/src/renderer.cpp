#include "fumar/render/renderer.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/core/log.hpp"
#include "fumar/platform/paths.hpp"
#include "fumar/platform/window.hpp"
#include "fumar/rhi/descriptor.hpp"
#include "fumar/rhi/device.hpp"
#include "fumar/rhi/frame_context.hpp"
#include "fumar/rhi/instance.hpp"
#include "fumar/rhi/pipeline.hpp"
#include "fumar/rhi/swapchain.hpp"
#include "fumar/render/gltf_loader.hpp"
#include "fumar/render/texture.hpp"
#include "fumar/rhi/upload_context.hpp"

#include <array>
#include <vector>

namespace fumar {
namespace {

/// Descriptor set 0, binding 0. Must match the CameraData block in mesh.vert.
///
/// alignas(16) reproduces the std140 rules the shader compiler applies: every
/// mat4 and vec4 starts on a 16-byte boundary. A struct that merely looks right
/// in C++ can still be laid out differently from the shader's view of it, and
/// the symptom is a scene that renders skewed rather than an error.
struct alignas(16) CameraUniforms {
    Mat4 view;
    Mat4 projection;
    Vec4 position;
};

/// Push constants, matching the block in mesh.vert. 64 bytes, comfortably
/// inside the 128 bytes every implementation guarantees.
struct ObjectPushConstants {
    Mat4 model;
};

constexpr bool kValidationByDefault =
#if defined(NDEBUG)
    false;
#else
    true;
#endif

constexpr vk::ImageSubresourceRange kWholeColorImage{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

constexpr vk::ImageSubresourceRange kWholeDepthImage{
    .aspectMask = vk::ImageAspectFlagBits::eDepth,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

} // namespace

Renderer::Renderer(Window& window) : m_window(window) {
    m_instance = std::make_unique<rhi::Instance>(rhi::InstanceDesc{
        .loader = window.vulkanLoaderFunction(),
        .requiredExtensions = window.requiredVulkanExtensions(),
        .applicationName = "fumar sandbox",
        .enableValidation = kValidationByDefault,
    });

    // The surface has to exist before the device is chosen: whether a queue
    // family can present is a property of the surface, not of the GPU alone.
    m_surface = window.createSurface(m_instance->handle());

    m_device = std::make_unique<rhi::Device>(*m_instance, m_surface);
    m_swapchain = std::make_unique<rhi::Swapchain>(*m_device, m_surface, window.framebufferSize());
    m_upload = std::make_unique<rhi::UploadContext>(*m_device);

    createDepthBuffer();
    createDefaultTexture();
    createDescriptors();

    const std::array<vk::VertexInputBindingDescription, 1> vertexBindings{Vertex::binding()};
    const auto vertexAttributes = Vertex::attributes();
    const std::array<vk::DescriptorSetLayout, 2> setLayouts{*m_cameraSetLayout, *m_materialSetLayout};

    const std::filesystem::path shaderDir = executableDirectory() / "shaders";
    m_pipeline = std::make_unique<rhi::GraphicsPipeline>(
        *m_device, rhi::GraphicsPipelineDesc{
                       .vertexShader = shaderDir / "mesh.vert.spv",
                       .fragmentShader = shaderDir / "mesh.frag.spv",
                       .colorFormat = m_swapchain->format(),
                       .depthFormat = m_depthFormat,
                       .vertexBindings = vertexBindings,
                       .vertexAttributes = vertexAttributes,
                       .setLayouts = setLayouts,
                       .pushConstantSize = sizeof(ObjectPushConstants),
                       .pushConstantStages = vk::ShaderStageFlagBits::eVertex,
                   });

    m_frames = std::make_unique<rhi::FrameContext>(*m_device, m_swapchain->imageCount());

    FUMAR_INFO("renderer ready");
}

Renderer::~Renderer() {
    // Nothing may be destroyed while the GPU might still be reading it, and the
    // GPU runs asynchronously, so the only safe first step is to wait.
    if (m_device) {
        m_device->waitIdle();
    }

    // Reverse order of creation. Spelled out because the members are a mix of
    // unique_ptr, RAII wrappers and plain handles, and only the first group
    // would order itself correctly.
    // Releases every mesh and texture the scene referenced. Must happen while
    // the device is alive, which is why it is here and not left to the member
    // destructors.
    m_resources.clear();
    for (PerFrame& frame : m_perFrame) {
        frame.cameraUniforms = rhi::Buffer{};
    }
    m_defaultTexture = rhi::Image{};
    m_depthImage = rhi::Image{};
    m_sampler.reset();
    m_materialSetLayout.reset();
    m_cameraSetLayout.reset();
    m_descriptorPool.reset();
    m_frames.reset();
    m_pipeline.reset();
    m_upload.reset();
    m_swapchain.reset();
    m_device.reset();

    if (m_surface) {
        m_window.destroySurface(m_instance->handle(), m_surface);
        m_surface = nullptr;
    }

    m_instance.reset();
    FUMAR_INFO("renderer shut down");
}

void Renderer::createDepthBuffer() {
    if (m_depthFormat == vk::Format::eUndefined) {
        // In preference order: 32-bit float depth first, then the combined
        // depth/stencil formats. One of these exists on every real GPU, but the
        // specification does not promise any single one of them.
        const std::array<vk::Format, 3> candidates{
            vk::Format::eD32Sfloat,
            vk::Format::eD32SfloatS8Uint,
            vk::Format::eD24UnormS8Uint,
        };
        m_depthFormat = m_device->findSupportedFormat(candidates, vk::ImageTiling::eOptimal,
                                                      vk::FormatFeatureFlagBits::eDepthStencilAttachment);
        FUMAR_VERIFY_MSG(m_depthFormat != vk::Format::eUndefined, "no usable depth format");
        FUMAR_INFO("depth format: {}", vk::to_string(m_depthFormat));
    }

    m_depthImage = rhi::Image(*m_device, rhi::ImageDesc{
                                             .extent = m_swapchain->extent(),
                                             .format = m_depthFormat,
                                             .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment,
                                             .aspect = vk::ImageAspectFlagBits::eDepth,
                                         });
}

void Renderer::createDefaultTexture() {
    // A checkerboard rather than flat white: it makes UV mapping, filtering and
    // texture orientation visible at a glance, which flat colour would hide.
    constexpr u32 kSize = 64;
    constexpr u32 kCheck = 8;

    std::vector<u8> pixels(static_cast<usize>(kSize) * kSize * 4);
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const bool light = ((x / kCheck) + (y / kCheck)) % 2 == 0;
            const u8 value = light ? 220 : 90;
            const usize index = (static_cast<usize>(y) * kSize + x) * 4;
            pixels[index + 0] = value;
            pixels[index + 1] = value;
            pixels[index + 2] = static_cast<u8>(light ? 235 : 110);
            pixels[index + 3] = 255;
        }
    }

    m_defaultTexture =
        rhi::Image(*m_device, rhi::ImageDesc{
                                  .extent = vk::Extent2D{kSize, kSize},
                                  // Srgb, so the hardware converts to linear on
                                  // sample and the lighting maths downstream
                                  // stays linear.
                                  .format = vk::Format::eR8G8B8A8Srgb,
                                  .usage = vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eTransferDst,
                                  .aspect = vk::ImageAspectFlagBits::eColor,
                              });

    m_upload->uploadImage(m_defaultTexture, pixels.data(), pixels.size());

    // The sampler is a separate object from the image: it describes how to READ
    // a texture (filtering, wrapping), not what is in it, so one sampler can
    // serve any number of textures.
    m_sampler = m_device->handle().createSamplerUnique(vk::SamplerCreateInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eLinear,
        .addressModeU = vk::SamplerAddressMode::eRepeat,
        .addressModeV = vk::SamplerAddressMode::eRepeat,
        .addressModeW = vk::SamplerAddressMode::eRepeat,
        .anisotropyEnable = VK_FALSE, // would need the samplerAnisotropy feature
        .minLod = 0.0f,
        .maxLod = 0.0f,
    });
}

void Renderer::createDescriptors() {
    const vk::Device handle = m_device->handle();

    m_cameraSetLayout = rhi::DescriptorSetLayoutBuilder()
                            .binding(0, vk::DescriptorType::eUniformBuffer,
                                     vk::ShaderStageFlagBits::eVertex)
                            .build(handle);

    m_materialSetLayout = rhi::DescriptorSetLayoutBuilder()
                              .binding(0, vk::DescriptorType::eCombinedImageSampler,
                                       vk::ShaderStageFlagBits::eFragment)
                              .build(handle);

    // One camera set per frame in flight, plus one material set per loaded
    // material. Pools do not grow, so the material budget is fixed up front -
    // a real asset system would allocate a pool per scene instead of guessing.
    constexpr u32 kMaterialBudget = 64;
    const std::array<vk::DescriptorPoolSize, 2> poolSizes{
        vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, rhi::kFramesInFlight},
        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, kMaterialBudget},
    };
    m_descriptorPool = std::make_unique<rhi::DescriptorPool>(
        *m_device, rhi::kFramesInFlight + kMaterialBudget, poolSizes);

    rhi::DescriptorWriter writer;

    for (PerFrame& frame : m_perFrame) {
        frame.cameraUniforms = rhi::Buffer(*m_device, rhi::BufferDesc{
                                                          .size = sizeof(CameraUniforms),
                                                          .usage = vk::BufferUsageFlagBits::eUniformBuffer,
                                                          // Rewritten every
                                                          // frame, so the CPU
                                                          // needs direct access.
                                                          .hostVisible = true,
                                                      });
        frame.cameraSet = m_descriptorPool->allocate(*m_cameraSetLayout);
        writer.buffer(frame.cameraSet, 0, frame.cameraUniforms.handle(), sizeof(CameraUniforms));
    }

    // The fallback material: used by anything with no material of its own, so
    // a broken or untextured asset shows an obvious checkerboard instead of
    // failing to bind a descriptor.
    Material fallback;
    fallback.name = "fallback";
    fallback.descriptorSet = m_descriptorPool->allocate(*m_materialSetLayout);
    writer.image(fallback.descriptorSet, 0, m_defaultTexture.view(), *m_sampler);
    m_resources.setFallbackMaterial(m_resources.addMaterial(std::move(fallback)));

    writer.submit(handle);
}

MeshHandle Renderer::createCubeMesh() {
    return m_resources.addMesh(makeCube(*m_device, *m_upload));
}

MeshHandle Renderer::createPlaneMesh(f32 halfSize, f32 uvTiling) {
    return m_resources.addMesh(makePlane(*m_device, *m_upload, halfSize, uvTiling));
}

MaterialHandle Renderer::createMaterial(std::string name, const std::filesystem::path& baseColorTexture) {
    Material material;
    material.name = std::move(name);

    vk::ImageView view = m_defaultTexture.view();
    if (!baseColorTexture.empty()) {
        rhi::Image texture = loadTextureFromFile(*m_device, *m_upload, baseColorTexture);
        if (texture.valid()) {
            material.baseColor = m_resources.addTexture(std::move(texture));
            view = m_resources.texture(material.baseColor).view();
        } else {
            FUMAR_WARN("material '{}' falls back to the checkerboard", material.name);
        }
    }

    material.descriptorSet = m_descriptorPool->allocate(*m_materialSetLayout);

    rhi::DescriptorWriter writer;
    writer.image(material.descriptorSet, 0, view, *m_sampler);
    writer.submit(m_device->handle());

    return m_resources.addMaterial(std::move(material));
}

NodeId Renderer::loadModel(const std::filesystem::path& path, NodeId parent) {
    const GltfLoadContext context{
        .device = *m_device,
        .upload = *m_upload,
        .descriptorPool = *m_descriptorPool,
        .materialSetLayout = *m_materialSetLayout,
        .sampler = *m_sampler,
        .fallbackTexture = m_defaultTexture.view(),
    };

    return loadGltfIntoScene(path, context, m_scene, m_resources, parent);
}

bool Renderer::recreateSwapchain() {
    if (!m_swapchain->recreate(m_window.framebufferSize())) {
        // Surface has no area - the window is minimised. The old swapchain is
        // untouched, so there is nothing to clean up; just try again later.
        return false;
    }

    // The depth buffer is sized to the colour attachment, so it has to follow.
    createDepthBuffer();

    // The number of images can change with the new size, and there is one
    // presentation semaphore per image.
    m_frames->onSwapchainRecreated(m_swapchain->imageCount());

    // The pipeline survives: viewport and scissor are dynamic state, and
    // neither attachment format has changed.
    return true;
}

void Renderer::updateCameraUniforms(u32 frameIndex) {
    const vk::Extent2D extent = m_swapchain->extent();
    const f32 aspect = static_cast<f32>(extent.width) / static_cast<f32>(extent.height);

    const CameraUniforms uniforms{
        .view = m_camera.view(),
        .projection = m_camera.projection(aspect),
        .position = point(m_camera.position),
    };

    // A plain memcpy into persistently mapped memory. No fence is needed: this
    // slot's previous frame was already waited on before we got here.
    m_perFrame[frameIndex].cameraUniforms.write(&uniforms, sizeof(uniforms));
}

void Renderer::recordCommands(u32 imageIndex) {
    const vk::CommandBuffer cmd = m_frames->commandBuffer();
    const vk::Extent2D extent = m_swapchain->extent();

    // --- barriers: get both attachments into writable layouts --------------
    const std::array<vk::ImageMemoryBarrier2, 2> toAttachment{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_swapchain->image(imageIndex),
            .subresourceRange = kWholeColorImage,
        },
        vk::ImageMemoryBarrier2{
            // Depth is tested early and written late, so both stages appear.
            .srcStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                            vk::PipelineStageFlagBits2::eLateFragmentTests,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                            vk::PipelineStageFlagBits2::eLateFragmentTests,
            .dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_depthImage.handle(),
            .subresourceRange = kWholeDepthImage,
        },
    };

    cmd.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(toAttachment.size()),
        .pImageMemoryBarriers = toAttachment.data(),
    });

    // --- rendering ----------------------------------------------------------
    vk::ClearValue colorClear{};
    colorClear.color.float32[0] = 0.05f;
    colorClear.color.float32[1] = 0.06f;
    colorClear.color.float32[2] = 0.09f;
    colorClear.color.float32[3] = 1.0f;

    vk::ClearValue depthClear{};
    // 1.0 is the far plane in Vulkan's 0..1 depth range, so clearing to it means
    // "nothing has been drawn here yet" and any geometry passes the eLess test.
    depthClear.depthStencil.depth = 1.0f;

    const vk::RenderingAttachmentInfo colorAttachment{
        .imageView = m_swapchain->imageView(imageIndex),
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = colorClear,
    };

    const vk::RenderingAttachmentInfo depthAttachment{
        .imageView = m_depthImage.view(),
        .imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        // eDontCare: depth is scratch space for this frame only, and telling the
        // driver we do not need it back lets it skip writing it out to memory.
        .storeOp = vk::AttachmentStoreOp::eDontCare,
        .clearValue = depthClear,
    };

    cmd.beginRendering(vk::RenderingInfo{
        .renderArea = vk::Rect2D{.offset = {0, 0}, .extent = extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachment,
        .pDepthAttachment = &depthAttachment,
    });

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_pipeline->handle());

    cmd.setViewport(0, vk::Viewport{
                           .x = 0.0f,
                           .y = 0.0f,
                           .width = static_cast<f32>(extent.width),
                           .height = static_cast<f32>(extent.height),
                           .minDepth = 0.0f,
                           .maxDepth = 1.0f,
                       });

    cmd.setScissor(0, vk::Rect2D{.offset = {0, 0}, .extent = extent});

    // Set 0 changes once per frame, set 1 once per material. Bound in that
    // order and left alone, which is exactly why they are separate sets.
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_pipeline->layout(), 0,
                           m_perFrame[m_frames->currentFrame()].cameraSet, {});
    // Materials are bound per drawable rather than once, because different
    // nodes use different ones. Consecutive nodes usually share a material
    // though, so the last one bound is remembered and the rebind skipped -
    // sorting the scene by material would take this further.
    vk::DescriptorSet boundMaterial;

    m_scene.forEachDrawable([&](const Node& node, const Mat4& worldTransform) {
        if (!m_resources.has(node.mesh)) {
            return;
        }

        const MaterialHandle handle =
            m_resources.has(node.material) ? node.material : m_resources.fallbackMaterial();
        if (!m_resources.has(handle)) {
            return;
        }

        const vk::DescriptorSet materialSet = m_resources.material(handle).descriptorSet;
        if (materialSet != boundMaterial) {
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_pipeline->layout(), 1, materialSet,
                                   {});
            boundMaterial = materialSet;
        }

        // The world transform comes straight from the scene's last update, so
        // this loop does no matrix work of its own.
        const ObjectPushConstants push{.model = worldTransform};
        cmd.pushConstants<ObjectPushConstants>(m_pipeline->layout(), vk::ShaderStageFlagBits::eVertex, 0,
                                               push);

        m_resources.mesh(node.mesh).draw(cmd);
    });

    cmd.endRendering();

    // --- barrier: colour attachment -> presentable -------------------------
    const vk::ImageMemoryBarrier2 toPresent{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        // The presentation engine is not a pipeline stage that can be waited
        // on; the semaphore handles that. This barrier only has to make the
        // writes visible and put the image in the right layout.
        .dstStageMask = vk::PipelineStageFlagBits2::eBottomOfPipe,
        .dstAccessMask = vk::AccessFlagBits2::eNone,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::ePresentSrcKHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = m_swapchain->image(imageIndex),
        .subresourceRange = kWholeColorImage,
    };

    cmd.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &toPresent,
    });
}

void Renderer::drawFrame() {
    if (m_window.minimized()) {
        return;
    }

    if (m_window.consumeResized()) {
        // The window system reports a size change on the very first frame, and
        // again for moves between monitors of equal scale. Rebuilding for a
        // size that has not actually changed just throws away a working
        // swapchain, so check before acting.
        if (m_swapchain->surfaceExtent() != m_swapchain->extent()) {
            m_swapchainDirty = true;
        }
    }

    if (m_swapchainDirty) {
        if (!recreateSwapchain()) {
            // Still minimised. Leave the flag set so the next frame retries.
            return;
        }
        m_swapchainDirty = false;
    }

    // Wait until the GPU is done with the frame that last used this slot, so
    // its command buffer, semaphores and uniform buffer are free to reuse.
    m_frames->waitForFrameSlot();

    const auto imageIndex = m_swapchain->acquireNextImage(m_frames->imageAvailable());
    if (!imageIndex) {
        // Swapchain no longer matches the surface. Flag it and skip this frame;
        // note the fence has NOT been reset yet, so nothing deadlocks.
        m_swapchainDirty = true;
        return;
    }

    m_frames->resetFence();

    // One pass over the tree, turning local transforms into world transforms.
    // Done here rather than lazily during recording so each node is computed
    // exactly once even when several nodes share a parent.
    m_scene.updateWorldTransforms();

    updateCameraUniforms(m_frames->currentFrame());

    m_frames->beginCommandBuffer();
    recordCommands(*imageIndex);
    m_frames->commandBuffer().end();

    // --- submit -------------------------------------------------------------
    const vk::CommandBufferSubmitInfo commandBufferInfo{
        .commandBuffer = m_frames->commandBuffer(),
    };

    // Wait for the image to actually be available, but only at the point where
    // colour is written. Everything before that - vertex shading, rasterising -
    // can start immediately, which is the whole point of a stage mask.
    const vk::SemaphoreSubmitInfo waitInfo{
        .semaphore = m_frames->imageAvailable(),
        .stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
    };

    const vk::SemaphoreSubmitInfo signalInfo{
        .semaphore = m_frames->renderFinished(*imageIndex),
        .stageMask = vk::PipelineStageFlagBits2::eAllGraphics,
    };

    const vk::SubmitInfo2 submitInfo{
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &waitInfo,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &commandBufferInfo,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signalInfo,
    };

    // The fence is signalled when this whole submission completes, which is
    // what waitForFrameSlot() blocks on two frames from now.
    m_device->graphicsQueue().submit2(submitInfo, m_frames->inFlightFence());

    if (!m_swapchain->present(*imageIndex, m_frames->renderFinished(*imageIndex))) {
        m_swapchainDirty = true;
    }

    m_frames->advance();
}

} // namespace fumar
