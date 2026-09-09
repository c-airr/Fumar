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

#include <algorithm>
#include <array>
#include <cmath>
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

/// Push constants, matching the block in mesh.vert and mesh.frag.
///
/// 84 bytes used of the 128 every implementation guarantees. Worth watching:
/// push constants are the fastest way to get per-draw data to a shader
/// precisely because the block is tiny and lives in the command buffer, so
/// anything that grows past the limit belongs in a uniform buffer instead.
struct ObjectPushConstants {
    Mat4 model;      // 64 bytes
    Vec4 baseColor;  // 16 bytes
    f32 highlight;   // 4 bytes: 0 = normal, 0.5 = hovered, 1 = selected
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

    createDefaultTexture();
    createViewportTarget(m_viewportExtent);
    createDescriptors();

    const std::array<vk::VertexInputBindingDescription, 1> vertexBindings{Vertex::binding()};
    const auto vertexAttributes = Vertex::attributes();
    const std::array<vk::DescriptorSetLayout, 2> setLayouts{*m_cameraSetLayout, *m_materialSetLayout};

    const std::filesystem::path shaderDir = executableDirectory() / "shaders";
    m_pipeline = std::make_unique<rhi::GraphicsPipeline>(
        *m_device, rhi::GraphicsPipelineDesc{
                       .vertexShader = shaderDir / "mesh.vert.spv",
                       .fragmentShader = shaderDir / "mesh.frag.spv",
                       // The scene pipeline targets the off-screen image, not
                       // the swapchain - so a change of window format never
                       // invalidates it.
                       .colorFormat = kSceneColorFormat,
                       .depthFormat = m_depthFormat,
                       .vertexBindings = vertexBindings,
                       .vertexAttributes = vertexAttributes,
                       .setLayouts = setLayouts,
                       .pushConstantSize = sizeof(ObjectPushConstants),
                       // Both stages: the vertex shader needs the model matrix,
                       // the fragment shader needs the colour and highlight.
                       // A range must cover every stage that reads it.
                       .pushConstantStages = vk::ShaderStageFlagBits::eVertex |
                                             vk::ShaderStageFlagBits::eFragment,
                   });

    m_outlinePipeline = std::make_unique<rhi::GraphicsPipeline>(
        *m_device, rhi::GraphicsPipelineDesc{
                       .vertexShader = shaderDir / "mesh.vert.spv",
                       .fragmentShader = shaderDir / "outline.frag.spv",
                       .colorFormat = kSceneColorFormat,
                       .depthFormat = m_depthFormat,
                       .vertexBindings = vertexBindings,
                       .vertexAttributes = vertexAttributes,
                       .setLayouts = setLayouts,
                       .pushConstantSize = sizeof(ObjectPushConstants),
                       .pushConstantStages = vk::ShaderStageFlagBits::eVertex |
                                             vk::ShaderStageFlagBits::eFragment,
                       // No culling: the far side of the wireframe should show
                       // through, which is what makes it read as a cage around
                       // the object rather than a half-drawn shell.
                       .cullMode = vk::CullModeFlagBits::eNone,
                       .polygonMode = vk::PolygonMode::eLine,
                       .lineWidth = m_device->wideLinesSupported() ? 2.0f : 1.0f,
                       .depthBiasConstant = -1.0f,
                       // Tested against the scene so the outline is hidden by
                       // objects in front of it, but not written, so it never
                       // occludes anything drawn later.
                       .depthWrite = false,
                       // Equal passes as well: the lines sit on the very
                       // surface they trace, and their depth values match it
                       // exactly.
                       .depthCompare = vk::CompareOp::eLessOrEqual,
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
    m_sceneColor = rhi::Image{};
    m_sampler.reset();
    m_materialSetLayout.reset();
    m_cameraSetLayout.reset();
    m_descriptorPool.reset();
    m_frames.reset();
    m_outlinePipeline.reset();
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

void Renderer::createViewportTarget(Extent2D size) {
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

    m_viewportExtent = size;
    const vk::Extent2D extent{size.width, size.height};

    m_sceneColor = rhi::Image(*m_device, rhi::ImageDesc{
                                             .extent = extent,
                                             .format = kSceneColorFormat,
                                             // eSampled is the whole point: the
                                             // interface reads this image back
                                             // as a texture to show in a panel.
                                             .usage = vk::ImageUsageFlagBits::eColorAttachment |
                                                      vk::ImageUsageFlagBits::eSampled,
                                             .aspect = vk::ImageAspectFlagBits::eColor,
                                         });

    m_depthImage = rhi::Image(*m_device, rhi::ImageDesc{
                                             .extent = extent,
                                             .format = m_depthFormat,
                                             .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment,
                                             .aspect = vk::ImageAspectFlagBits::eDepth,
                                         });
}

bool Renderer::resizeViewport(Extent2D size) {
    // A panel can report zero size while it is being dragged or is collapsed,
    // and rebuilding for every pixel of a drag would stall the GPU on every
    // frame of it. Neither is worth doing.
    if (size.width == 0 || size.height == 0 || size == m_viewportExtent) {
        return false;
    }

    // The old images may still be referenced by frames in flight.
    m_device->waitIdle();
    createViewportTarget(size);

    FUMAR_DEBUG("viewport target resized to {}x{}", size.width, size.height);
    return true;
}

void Renderer::createDefaultTexture() {
    // A single white pixel.
    //
    // Every material samples a texture, and one that has no texture of its own
    // uses this: white multiplied by the material colour is the material
    // colour. That keeps the shader branch-free, at the cost of one sample
    // whose result is known - a trade every engine makes.
    const std::array<u8, 4> white{255, 255, 255, 255};

    m_defaultTexture = rhi::Image(*m_device, rhi::ImageDesc{
                                                 .extent = vk::Extent2D{1, 1},
                                                 .format = vk::Format::eR8G8B8A8Srgb,
                                                 .usage = vk::ImageUsageFlagBits::eSampled |
                                                          vk::ImageUsageFlagBits::eTransferDst,
                                                 .aspect = vk::ImageAspectFlagBits::eColor,
                                             });

    m_upload->uploadImage(m_defaultTexture, white.data(), white.size());

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
    fallback.name = "default";
    fallback.baseColorFactor = Vec4{0.62f, 0.63f, 0.65f, 1.0f};
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

MeshHandle Renderer::createCylinderMesh(f32 radius, f32 height, u32 segments) {
    return m_resources.addMesh(makeCylinder(*m_device, *m_upload, radius, height, segments));
}

MaterialHandle Renderer::createMaterial(std::string name, Vec4 baseColor,
                                        const std::filesystem::path& baseColorTexture) {
    Material material;
    material.name = std::move(name);
    material.baseColorFactor = baseColor;

    vk::ImageView view = m_defaultTexture.view();
    if (!baseColorTexture.empty()) {
        rhi::Image texture = loadTextureFromFile(*m_device, *m_upload, baseColorTexture);
        if (texture.valid()) {
            material.baseColor = m_resources.addTexture(std::move(texture));
            view = m_resources.texture(material.baseColor).view();
        } else {
            FUMAR_WARN("material '{}' falls back to a flat colour", material.name);
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

namespace {

/// Slab test: how far along the ray it enters and leaves an axis-aligned box.
///
/// For each axis the box is a pair of parallel planes; the ray is inside the
/// box only where all three intervals overlap. Returns false when they do not,
/// or when the overlap lies entirely behind the ray origin.
bool intersectRayBounds(const Vec3& origin, const Vec3& direction, const Bounds& bounds, f32& outDistance) {
    f32 tMin = 0.0f;
    f32 tMax = 1e30f;

    for (usize axis = 0; axis < 3; ++axis) {
        const f32 d = direction[axis];
        const f32 o = origin[axis];
        const f32 lo = bounds.min[axis];
        const f32 hi = bounds.max[axis];

        if (std::abs(d) < 1e-8f) {
            // Parallel to this pair of planes: either always inside them or
            // never, and no division is possible.
            if (o < lo || o > hi) {
                return false;
            }
            continue;
        }

        f32 t1 = (lo - o) / d;
        f32 t2 = (hi - o) / d;
        if (t1 > t2) {
            std::swap(t1, t2);
        }

        tMin = t1 > tMin ? t1 : tMin;
        tMax = t2 < tMax ? t2 : tMax;
        if (tMin > tMax) {
            return false;
        }
    }

    outDistance = tMin;
    return true;
}

} // namespace

NodeId Renderer::pickNode(const Ray& ray) const {
    NodeId best = kInvalidNode;
    f32 bestDistance = 1e30f;

    m_scene.forEachDrawable([&](NodeId id, const Node& node, const Mat4& worldTransform) {
        if (!m_resources.has(node.mesh)) {
            return;
        }

        // Into the object own space, where its bounding box really is axis
        // aligned. A direction is transformed with w = 0 so translation does
        // not apply to it, and is deliberately left unnormalised: scaling has
        // to affect it for the resulting distance to stay comparable.
        const Mat4 toLocal = inverse(worldTransform);
        const Vec3 localOrigin = xyz(toLocal * point(ray.origin));
        const Vec3 localDirection = xyz(toLocal * direction(ray.direction));

        f32 distance = 0.0f;
        if (!intersectRayBounds(localOrigin, localDirection, m_resources.mesh(node.mesh).bounds(),
                                distance)) {
            return;
        }

        if (distance < bestDistance) {
            bestDistance = distance;
            best = id;
        }
    });

    return best;
}

void Renderer::waitIdle() const {
    if (m_device) {
        m_device->waitIdle();
    }
}

vk::Format Renderer::swapchainFormat() const {
    return m_swapchain->format();
}

u32 Renderer::swapchainImageCount() const {
    return m_swapchain->imageCount();
}

bool Renderer::recreateSwapchain() {
    if (!m_swapchain->recreate(m_window.framebufferSize())) {
        // Surface has no area - the window is minimised. The old swapchain is
        // untouched, so there is nothing to clean up; just try again later.
        return false;
    }

    // Nothing else follows the swapchain now: the scene draws into the
    // off-screen target, sized by the viewport panel rather than the window.

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

void Renderer::recordSceneRendering(vk::CommandBuffer cmd) {
    const vk::Extent2D extent{m_viewportExtent.width, m_viewportExtent.height};

    // --- barriers: both attachments into writable layouts -------------------
    const std::array<vk::ImageMemoryBarrier2, 2> toAttachment{
        vk::ImageMemoryBarrier2{
            // Waits for the fragment shader that sampled this image last frame,
            // when the interface displayed it. Overwriting it before that read
            // completes is exactly the hazard this barrier exists for.
            .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_sceneColor.handle(),
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

    vk::ClearValue colorClear{};
    colorClear.color.float32[0] = 0.055f;
    colorClear.color.float32[1] = 0.058f;
    colorClear.color.float32[2] = 0.065f;
    colorClear.color.float32[3] = 1.0f;

    vk::ClearValue depthClear{};
    // 1.0 is the far plane in Vulkan 0..1 depth, so clearing to it means
    // nothing has been drawn here yet and any geometry passes the eLess test.
    depthClear.depthStencil.depth = 1.0f;

    const vk::RenderingAttachmentInfo colorAttachment{
        .imageView = m_sceneColor.view(),
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = colorClear,
    };

    const vk::RenderingAttachmentInfo depthAttachment{
        .imageView = m_depthImage.view(),
        .imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        // eDontCare: depth is scratch space for this frame only, and saying we
        // do not need it back lets the driver skip writing it out to memory.
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

    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_pipeline->layout(), 0,
                           m_perFrame[m_frames->currentFrame()].cameraSet, {});

    // Materials are bound per drawable rather than once, because different
    // nodes use different ones. Consecutive nodes usually share a material
    // though, so the last one bound is remembered and the rebind skipped -
    // sorting the scene by material would take this further.
    vk::DescriptorSet boundMaterial;

    m_scene.forEachDrawable([&](NodeId id, const Node& node, const Mat4& worldTransform) {
        if (!m_resources.has(node.mesh)) {
            return;
        }

        const MaterialHandle handle =
            m_resources.has(node.material) ? node.material : m_resources.fallbackMaterial();
        if (!m_resources.has(handle)) {
            return;
        }

        const Material& material = m_resources.material(handle);
        if (material.descriptorSet != boundMaterial) {
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_pipeline->layout(), 1,
                                   material.descriptorSet, {});
            boundMaterial = material.descriptorSet;
        }

        // The world transform comes straight from the last scene update, so
        // this loop does no matrix work of its own.
        // Only the selection tints its faces. Hovering is answered by the
        // wireframe pass below and nothing else, so passing the cursor over a
        // crowded scene does not make objects flash.
        const ObjectPushConstants push{
            .model = worldTransform,
            .baseColor = material.baseColorFactor,
            .highlight = id == m_selected ? 1.0f : 0.0f,
        };
        cmd.pushConstants<ObjectPushConstants>(
            m_pipeline->layout(),
            vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, push);

        m_resources.mesh(node.mesh).draw(cmd);
    });

    // --- outlines -----------------------------------------------------------
    // Drawn last, so they sit over the geometry rather than being overwritten
    // by whatever was drawn afterwards. Same pass: starting another one would
    // mean storing and reloading the whole colour attachment for two objects.
    const auto drawOutline = [&](NodeId id, f32 strength) {
        if (id == kInvalidNode || !m_scene.isAlive(id)) {
            return;
        }
        const Node& node = m_scene.node(id);
        if (!node.visible || !m_resources.has(node.mesh)) {
            return;
        }

        const ObjectPushConstants push{
            .model = m_scene.worldTransform(id),
            .baseColor = Vec4{1.0f, 1.0f, 1.0f, 1.0f},
            .highlight = strength,
        };
        cmd.pushConstants<ObjectPushConstants>(
            m_outlinePipeline->layout(),
            vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, push);

        m_resources.mesh(node.mesh).draw(cmd);
    };

    if (m_highlighted != kInvalidNode || m_selected != kInvalidNode) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_outlinePipeline->handle());

        // The outline shader ignores both sets, but the layout still declares
        // them, so something compatible has to be bound or the draw is invalid.
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_outlinePipeline->layout(), 0,
                               m_perFrame[m_frames->currentFrame()].cameraSet, {});
        if (m_resources.has(m_resources.fallbackMaterial())) {
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_outlinePipeline->layout(), 1,
                                   m_resources.material(m_resources.fallbackMaterial()).descriptorSet, {});
        }

        // Hover first, so that when the same object is both, the brighter
        // selected outline is what ends up on top.
        if (m_highlighted != m_selected) {
            drawOutline(m_highlighted, 0.5f);
        }
        drawOutline(m_selected, 1.0f);
    }

    cmd.endRendering();

    // The interface samples this image in its fragment shader, so it has to be
    // readable by the time that runs.
    const vk::ImageMemoryBarrier2 toShaderRead{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = m_sceneColor.handle(),
        .subresourceRange = kWholeColorImage,
    };

    cmd.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &toShaderRead,
    });
}

void Renderer::recordUiRendering(vk::CommandBuffer cmd, u32 imageIndex) {
    const vk::Extent2D extent = m_swapchain->extent();

    const vk::ImageMemoryBarrier2 toColorAttachment{
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
    };

    cmd.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &toColorAttachment,
    });

    vk::ClearValue clear{};
    clear.color.float32[0] = 0.043f;
    clear.color.float32[1] = 0.047f;
    clear.color.float32[2] = 0.055f;
    clear.color.float32[3] = 1.0f;

    const vk::RenderingAttachmentInfo colorAttachment{
        .imageView = m_swapchain->imageView(imageIndex),
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = clear,
    };

    // No depth attachment: the interface is drawn back to front in submission
    // order and has no use for depth testing.
    cmd.beginRendering(vk::RenderingInfo{
        .renderArea = vk::Rect2D{.offset = {0, 0}, .extent = extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachment,
    });

    if (m_overlay) {
        m_overlay(cmd);
    }

    cmd.endRendering();

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

void Renderer::recordCommands(u32 imageIndex) {
    const vk::CommandBuffer cmd = m_frames->commandBuffer();

    // Two passes: the scene into the off-screen target, then the interface into
    // the window with that target as one of its textures. They cannot share a
    // pass, because the second reads what the first wrote.
    recordSceneRendering(cmd);
    recordUiRendering(cmd, imageIndex);
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
