#include "fumar/render/renderer.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/core/log.hpp"
#include "fumar/geometry/primitives.hpp"
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
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace fumar {
namespace {

/// Descriptor set 0, binding 0. Must match the FrameData block in
/// shaders/frame.glsl exactly.
///
/// alignas(16) reproduces the std140 rules the shader compiler applies: every
/// mat4 and vec4 starts on a 16-byte boundary. A struct that merely looks right
/// in C++ can still be laid out differently from the shader's view of it, and
/// the symptom is a scene that renders skewed rather than an error.
///
/// The four floats at the end are deliberate: std140 packs scalars tightly, so
/// grouping them means they share one 16-byte slot instead of taking four.
/// One light, laid out exactly as SceneLight in shaders/frame.glsl.
///
/// Four vec4s rather than named scalars for one reason: std140 rounds every
/// element of an array of structs up to a multiple of sixteen bytes, so a
/// struct written out plainly - a vec3, a float, a vec3, a float - would occupy
/// twice the space for the same numbers.
struct alignas(16) LightUniform {
    Vec4 positionRange;
    Vec4 colorIntensity;
    Vec4 directionOuter;
    Vec4 shape;
};

/// Must match kMaxLights in shaders/frame.glsl.
constexpr u32 kMaxLights = 16;

/// How many instances the per-frame record buffer is sized for. Growing it
/// would mean reallocating and rewriting a descriptor mid-frame; a fixed
/// ceiling that matches the acceleration structure's own is simpler and the
/// consequence of exceeding it is only that the extra objects do not appear in
/// reflections.
constexpr u32 kMaxInstances = 1024;

/// How many textures a ray can reach. Must match kMaxSceneTextures in
/// shaders/raytrace.glsl.
///
/// A fixed array rather than one sized at runtime: a variable-count descriptor
/// binding is another feature to require and another way for an older driver to
/// say no, and every unused slot here costs one descriptor. Slots past the end
/// of the scene hold the default texture, so a stale index samples white rather
/// than reading a descriptor nobody wrote.
constexpr u32 kMaxSceneTextures = 128;

/// How many times the bloom chain halves the image.
///
/// Six levels starting at half resolution means the smallest one is 1/64th of
/// the viewport, and a blur of one texel there reaches across a good part of
/// the screen. That is what sets how far the glow can spread: more levels buy
/// reach nobody wants, fewer make a bright sky glow like a bright lamp.
constexpr u32 kBloomMips = 6;

/// Push constants for one downsample step. Matches bloom_down.frag.
struct BloomDownPush {
    Vec2 texelSize;
    f32 threshold;
    f32 knee;
    i32 prefilter;
};

/// Push constants for one upsample step. Matches bloom_up.frag.
struct BloomUpPush {
    Vec2 texelSize;
    f32 radius;
};

struct alignas(16) FrameUniforms {
    Mat4 view;
    Mat4 projection;
    Mat4 invViewProjection;
    Mat4 prevViewProjection;
    Vec4 cameraPosition;
    Vec4 prevCameraPosition;
    Vec4 sunDirection;
    Vec4 sunColor;
    Vec4 skyZenithColor;
    Vec4 skyHorizonColor;
    Vec4 groundColor;
    f32 sunIntensity;
    f32 sunAngularRadius;
    f32 skyIntensity;
    f32 exposure;
    f32 shadowStrength;
    f32 occlusionStrength;
    f32 occlusionRadius;
    f32 reflectionStrength;
    f32 reflectionRoughnessLimit;
    f32 indirectStrength;
    f32 bloomStrength;
    f32 bloomThreshold;
    f32 temporalStrength;
    i32 frameIndex;
    i32 lightCount;
    std::array<LightUniform, kMaxLights> lights;
};

/// How many frames the sampling pattern takes to come back round.
///
/// The pattern is advanced by the golden ratio each frame, which never repeats
/// - but the index it is multiplied by cannot grow without bound or it loses
/// precision as a float. Wrapping at 64 repeats a set of 64 rotations that are
/// still evenly spread, and the temporal average only ever reaches about twenty
/// frames back, so nothing lives long enough to see the repeat.
constexpr u32 kSamplePatternPeriod = 64;

/// Push constants, matching the block in shaders/object.glsl.
///
/// 100 bytes used of the 128 every implementation guarantees. Worth watching:
/// push constants are the fastest way to get per-draw data to a shader
/// precisely because the block is tiny and lives in the command buffer, so
/// anything that grows past the limit belongs in a uniform buffer instead.
struct ObjectPushConstants {
    Mat4 model;      // 64 bytes, offset 0
    Vec4 baseColor;  // 16,       offset 64

    /// Before the scalars, not after them, and the order is load-bearing.
    ///
    /// Push constants follow std430, where a vec2 is aligned to EIGHT bytes.
    /// Placed after three floats it would start at offset 92, which is not a
    /// multiple of 8, so the shader compiler inserts four bytes of padding and
    /// reads it from 96 - while C++, which aligns a pair of floats to four,
    /// writes it at 92. Nothing warns: the object simply gets somebody else's
    /// tiling. Here it lands at 80, which both agree on.
    Vec2 uvScale;    // 8,        offset 80

    f32 metallic;    // 4,        offset 88
    f32 roughness;   // 4,        offset 92
    f32 highlight;   // 4,        offset 96: 0 = normal, 0.5 = hovered, 1 = selected
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

    // Same shading, two builds of it: the ray query variant traces for shadows
    // and occlusion, the plain one assumes everything is lit and open. Choosing
    // between compiled shaders rather than branching inside one keeps the
    // fallback free of any cost, and is the only option anyway - an extension
    // has to be declared when the shader is compiled, not when it runs.
    const std::filesystem::path meshFragment = m_device->rayTracingSupported()
                                                   ? shaderDir / "mesh_rq.frag.spv"
                                                   : shaderDir / "mesh.frag.spv";

    m_pipeline = std::make_unique<rhi::GraphicsPipeline>(
        *m_device, rhi::GraphicsPipelineDesc{
                       .vertexShader = shaderDir / "mesh.vert.spv",
                       .fragmentShader = meshFragment,
                       // The scene pipeline targets the off-screen HDR image,
                       // not the swapchain - so a change of window format never
                       // invalidates it.
                       .colorFormat = kSceneHdrFormat,
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
                       // Must match the attachments of the pass this draws
                       // into. A pipeline built for one sample simply cannot
                       // render into a multisampled target.
                       .samples = m_sampleCount,
                   });

    m_outlinePipeline = std::make_unique<rhi::GraphicsPipeline>(
        *m_device, rhi::GraphicsPipelineDesc{
                       .vertexShader = shaderDir / "mesh.vert.spv",
                       .fragmentShader = shaderDir / "outline.frag.spv",
                       .colorFormat = kSceneHdrFormat,
                       .depthFormat = m_depthFormat,
                       .vertexBindings = vertexBindings,
                       .vertexAttributes = vertexAttributes,
                       .setLayouts = setLayouts,
                       .pushConstantSize = sizeof(ObjectPushConstants),
                       .pushConstantStages = vk::ShaderStageFlagBits::eVertex |
                                             vk::ShaderStageFlagBits::eFragment,
                       .samples = m_sampleCount,
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

    // The sky and the tone mapper are both fullscreen passes: three vertices
    // generated in the vertex shader, no vertex buffer, no geometry. Note the
    // empty vertexBindings/vertexAttributes - that is what says so.
    m_skyPipeline = std::make_unique<rhi::GraphicsPipeline>(
        *m_device, rhi::GraphicsPipelineDesc{
                       .vertexShader = shaderDir / "fullscreen.vert.spv",
                       .fragmentShader = shaderDir / "sky.frag.spv",
                       .colorFormat = kSceneHdrFormat,
                       // Declared even though nothing here touches depth: a
                       // pipeline used in a pass that has a depth attachment
                       // must name its format, or creation fails.
                       .depthFormat = m_depthFormat,
                       // Empty: fullscreen.vert builds its three vertices from
                       // gl_VertexIndex, so there is no vertex buffer to
                       // describe and none is bound before the draw.
                       .vertexBindings = {},
                       .vertexAttributes = {},
                       .setLayouts = setLayouts,
                       .samples = m_sampleCount,
                       .cullMode = vk::CullModeFlagBits::eNone,
                       // The sky is behind everything, so it neither tests
                       // depth (nothing has been drawn yet) nor writes it
                       // (geometry drawn afterwards must not be rejected).
                       .depthTest = false,
                       .depthWrite = false,
                   });

    // The tone mapper reads two images, so it gets its own set layout rather
    // than the material one every other fullscreen pass borrows.
    const std::array<vk::DescriptorSetLayout, 2> postSetLayouts{*m_cameraSetLayout,
                                                                *m_postSetLayout};

    m_tonemapPipeline = std::make_unique<rhi::GraphicsPipeline>(
        *m_device, rhi::GraphicsPipelineDesc{
                       .vertexShader = shaderDir / "fullscreen.vert.spv",
                       .fragmentShader = shaderDir / "tonemap.frag.spv",
                       // This one writes the displayable image, so it is the
                       // only scene pipeline built for the 8-bit sRGB format.
                       .colorFormat = kSceneColorFormat,
                       .vertexBindings = {},
                       .vertexAttributes = {},
                       .setLayouts = postSetLayouts,
                       .cullMode = vk::CullModeFlagBits::eNone,
                       .depthTest = false,
                       .depthWrite = false,
                   });

    // --- the temporal filter ------------------------------------------------
    // Set 0 is the frame data, for this frame's camera and last frame's; set 1
    // is the three images it reads. No depth attachment and no push constants:
    // everything it needs is in those two sets.
    const std::array<vk::DescriptorSetLayout, 2> temporalSetLayouts{*m_cameraSetLayout,
                                                                    *m_temporalSetLayout};

    m_temporalPipeline = std::make_unique<rhi::GraphicsPipeline>(
        *m_device, rhi::GraphicsPipelineDesc{
                       .vertexShader = shaderDir / "fullscreen.vert.spv",
                       .fragmentShader = shaderDir / "temporal.frag.spv",
                       // One sample, unlike the scene pipelines above: the
                       // multisampling is already resolved by the time this
                       // runs, and what it writes is read as a texture.
                       .colorFormat = kSceneHdrFormat,
                       .vertexBindings = {},
                       .vertexAttributes = {},
                       .setLayouts = temporalSetLayouts,
                       .cullMode = vk::CullModeFlagBits::eNone,
                       .depthTest = false,
                       .depthWrite = false,
                   });

    // --- the bloom chain ----------------------------------------------------
    // One source image, one target, no frame data: everything these two need
    // arrives in push constants, so they take a single set layout - the
    // material one, which is exactly "one texture" - and it lands as set 0.
    const std::array<vk::DescriptorSetLayout, 1> bloomSetLayouts{*m_materialSetLayout};

    m_bloomDownPipeline = std::make_unique<rhi::GraphicsPipeline>(
        *m_device, rhi::GraphicsPipelineDesc{
                       .vertexShader = shaderDir / "fullscreen.vert.spv",
                       .fragmentShader = shaderDir / "bloom_down.frag.spv",
                       .colorFormat = kSceneHdrFormat,
                       .vertexBindings = {},
                       .vertexAttributes = {},
                       .setLayouts = bloomSetLayouts,
                       .pushConstantSize = sizeof(BloomDownPush),
                       .pushConstantStages = vk::ShaderStageFlagBits::eFragment,
                       .cullMode = vk::CullModeFlagBits::eNone,
                       .depthTest = false,
                       .depthWrite = false,
                   });

    m_bloomUpPipeline = std::make_unique<rhi::GraphicsPipeline>(
        *m_device, rhi::GraphicsPipelineDesc{
                       .vertexShader = shaderDir / "fullscreen.vert.spv",
                       .fragmentShader = shaderDir / "bloom_up.frag.spv",
                       .colorFormat = kSceneHdrFormat,
                       .vertexBindings = {},
                       .vertexAttributes = {},
                       .setLayouts = bloomSetLayouts,
                       .pushConstantSize = sizeof(BloomUpPush),
                       .pushConstantStages = vk::ShaderStageFlagBits::eFragment,
                       // The one pipeline in the engine that blends: each level
                       // is ADDED to the one above it, and a fragment shader
                       // cannot read the attachment it is writing.
                       .additiveBlend = true,
                       .cullMode = vk::CullModeFlagBits::eNone,
                       .depthTest = false,
                       .depthWrite = false,
                   });

    m_frames = std::make_unique<rhi::FrameContext>(*m_device, m_swapchain->imageCount());

    // A unit cube, scaled down when it is drawn. Kept out of the resource
    // registry on purpose: it belongs to the editor's presentation of the
    // scene, not to the scene, and would otherwise be saved into scene files
    // and listed in the content panel as though somebody had made it.
    m_lightMarker = makeMesh(*m_device, *m_upload, geometry::makeCube());

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
        frame.instanceData = rhi::Buffer{};
        frame.topLevel = rhi::TopLevelStructure{};
    }
    m_retiredMeshes.clear();
    m_lightMarker = Mesh{};
    m_defaultTexture = rhi::Image{};
    m_depthImage = rhi::Image{};
    m_depthResolved = rhi::Image{};
    m_sceneHdr = rhi::Image{};
    m_sceneHdrMs = rhi::Image{};
    m_sceneColor = rhi::Image{};
    for (rhi::Image& image : m_accum) {
        image = rhi::Image{};
    }
    m_bloomMipViews.clear();
    m_bloom = rhi::Image{};
    m_pointSampler.reset();
    m_clampSampler.reset();
    m_sampler.reset();
    m_temporalSetLayout.reset();
    m_postSetLayout.reset();
    m_materialSetLayout.reset();
    m_cameraSetLayout.reset();
    m_descriptorPool.reset();
    m_frames.reset();
    m_bloomUpPipeline.reset();
    m_bloomDownPipeline.reset();
    m_temporalPipeline.reset();
    m_tonemapPipeline.reset();
    m_skyPipeline.reset();
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
        // eSampledImage as well as the attachment bit, because the temporal
        // pass READS the depth buffer to turn a pixel back into a point in the
        // world. Sampling a depth format is not something Vulkan promises for
        // every one of them, so it goes into the search rather than being
        // discovered later as a validation error.
        m_depthFormat = m_device->findSupportedFormat(
            candidates, vk::ImageTiling::eOptimal,
            vk::FormatFeatureFlagBits::eDepthStencilAttachment |
                vk::FormatFeatureFlagBits::eSampledImage);
        FUMAR_VERIFY_MSG(m_depthFormat != vk::Format::eUndefined, "no usable depth format");
        FUMAR_INFO("depth format: {}", vk::to_string(m_depthFormat));
    }

    if (m_sampleCount == vk::SampleCountFlagBits::e1) {
        m_sampleCount = chooseSampleCount();
        FUMAR_INFO("anti-aliasing: {}x MSAA", static_cast<u32>(m_sampleCount));
    }

    m_viewportExtent = size;
    const vk::Extent2D extent{size.width, size.height};

    // Where the geometry lands. eSampled because the tone mapping pass reads
    // it back as a texture in the very next pass of the same frame.
    m_sceneHdr = rhi::Image(*m_device, rhi::ImageDesc{
                                           .extent = extent,
                                           .format = kSceneHdrFormat,
                                           .usage = vk::ImageUsageFlagBits::eColorAttachment |
                                                    vk::ImageUsageFlagBits::eSampled,
                                           .aspect = vk::ImageAspectFlagBits::eColor,
                                       });

    // What the interface displays, after tone mapping. eSampled for the same
    // reason one step further along: ImGui reads it as a texture.
    m_sceneColor = rhi::Image(*m_device, rhi::ImageDesc{
                                             .extent = extent,
                                             .format = kSceneColorFormat,
                                             .usage = vk::ImageUsageFlagBits::eColorAttachment |
                                                      vk::ImageUsageFlagBits::eSampled,
                                             .aspect = vk::ImageAspectFlagBits::eColor,
                                         });

    // Where the triangles actually land. Several samples per pixel, resolved
    // into m_sceneHdr when the pass ends.
    //
    // A triangle edge does not fall on a pixel boundary, so a pixel it half
    // covers is either fully shaded or not at all - which is what makes an edge
    // a staircase. Multisampling keeps coverage per SAMPLE while still shading
    // once per pixel, so the edge pixel ends up as a weighted mix of the
    // triangle and what is behind it. Almost all of the quality, a fraction of
    // the cost of rendering four times the pixels.
    //
    // eTransientAttachment because nothing ever reads this image: it is written
    // and resolved inside one pass, so on hardware that can, it need never
    // reach memory at all.
    if (m_sampleCount != vk::SampleCountFlagBits::e1) {
        m_sceneHdrMs = rhi::Image(*m_device, rhi::ImageDesc{
                                                 .extent = extent,
                                                 .format = kSceneHdrFormat,
                                                 .usage = vk::ImageUsageFlagBits::eColorAttachment |
                                                          vk::ImageUsageFlagBits::eTransientAttachment,
                                                 .aspect = vk::ImageAspectFlagBits::eColor,
                                                 .samples = m_sampleCount,
                                             });
    }

    // With anti-aliasing on, the depth attachment is multisampled and cannot be
    // sampled by a shader at all, so it is resolved into the image below and
    // this one needs no eSampled. Without it, this IS the image the temporal
    // pass reads.
    const bool multisampled = m_sampleCount != vk::SampleCountFlagBits::e1;

    m_depthImage = rhi::Image(*m_device, rhi::ImageDesc{
                                             .extent = extent,
                                             .format = m_depthFormat,
                                             .usage = multisampled
                                                          ? vk::ImageUsageFlags{vk::ImageUsageFlagBits::eDepthStencilAttachment}
                                                          : vk::ImageUsageFlagBits::eDepthStencilAttachment |
                                                                vk::ImageUsageFlagBits::eSampled,
                                             .aspect = vk::ImageAspectFlagBits::eDepth,
                                             // Matched to the colour target:
                                             // the two are attachments of the
                                             // same pass and a mismatch is
                                             // invalid.
                                             .samples = m_sampleCount,
                                         });

    if (multisampled) {
        m_depthResolved = rhi::Image(*m_device,
                                     rhi::ImageDesc{
                                         .extent = extent,
                                         .format = m_depthFormat,
                                         .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                                                  vk::ImageUsageFlagBits::eSampled,
                                         .aspect = vk::ImageAspectFlagBits::eDepth,
                                     });
    }

    // The pair the temporal filter alternates between. Both are written as
    // colour attachments and read as textures, and neither is transient: the
    // whole point is that one of them survives into the next frame.
    for (rhi::Image& image : m_accum) {
        image = rhi::Image(*m_device, rhi::ImageDesc{
                                          .extent = extent,
                                          .format = kSceneHdrFormat,
                                          .usage = vk::ImageUsageFlagBits::eColorAttachment |
                                                   vk::ImageUsageFlagBits::eSampled,
                                          .aspect = vk::ImageAspectFlagBits::eColor,
                                      });
    }

    // Brand new images hold uninitialised memory, not a picture of anything.
    // One frame of running without history, and then there is some.
    m_historyValid = false;

    createBloomChain();
}

const rhi::Image& Renderer::sampledDepth() const {
    return m_depthResolved.valid() ? m_depthResolved : m_depthImage;
}

vk::SampleCountFlagBits Renderer::chooseSampleCount() const {
    // Four is the sweet spot everyone lands on. Two leaves a visible staircase
    // on a near-vertical edge, and eight costs twice the bandwidth of four for
    // a difference that needs a still frame and a magnifier.
    //
    // The two masks are intersected because colour and depth are attachments of
    // the same pass: a count either works for both or cannot be used.
    const vk::SampleCountFlags supported = m_device->properties().limits.framebufferColorSampleCounts &
                                           m_device->properties().limits.framebufferDepthSampleCounts;

    if (supported & vk::SampleCountFlagBits::e4) {
        return vk::SampleCountFlagBits::e4;
    }
    if (supported & vk::SampleCountFlagBits::e2) {
        return vk::SampleCountFlagBits::e2;
    }
    return vk::SampleCountFlagBits::e1;
}

vk::Extent2D Renderer::bloomMipExtent(u32 level) const {
    // Half the viewport to start with - the glow is blurry by definition, so
    // the sharp resolution is detail nobody can see and bandwidth nobody gets
    // back - then halved per level, never below one texel.
    return vk::Extent2D{
        std::max(1u, (m_viewportExtent.width / 2) >> level),
        std::max(1u, (m_viewportExtent.height / 2) >> level),
    };
}

void Renderer::createBloomChain() {
    const vk::Extent2D base = bloomMipExtent(0);

    // A mip chain cannot have more levels than the image can be halved, and a
    // small viewport panel runs out before six. Asking for more is not a
    // warning but an invalid image.
    const u32 largest = std::max(base.width, base.height);
    m_bloomMipCount = std::min(kBloomMips, static_cast<u32>(std::floor(std::log2(largest))) + 1u);

    m_bloom = rhi::Image(*m_device, rhi::ImageDesc{
                                        .extent = base,
                                        .format = kSceneHdrFormat,
                                        .usage = vk::ImageUsageFlagBits::eColorAttachment |
                                                 vk::ImageUsageFlagBits::eSampled,
                                        .aspect = vk::ImageAspectFlagBits::eColor,
                                        .mipLevels = m_bloomMipCount,
                                    });

    // One view per level. The image's own view spans the whole chain, which is
    // the wrong thing to render into - a colour attachment is exactly one level
    // - and the wrong thing to sample, since a pass wants the level it names
    // and not a filtered blend of it with its neighbours.
    m_bloomMipViews.clear();
    m_bloomMipViews.reserve(m_bloomMipCount);
    for (u32 level = 0; level < m_bloomMipCount; ++level) {
        m_bloomMipViews.push_back(m_device->handle().createImageViewUnique(vk::ImageViewCreateInfo{
            .image = m_bloom.handle(),
            .viewType = vk::ImageViewType::e2D,
            .format = kSceneHdrFormat,
            .subresourceRange =
                vk::ImageSubresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
        }));
    }
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

    // createViewportTarget replaced every off-screen image, so the descriptors
    // the passes after the scene read them through now point at freed views.
    // Safe to rewrite here and only here, because of the waitIdle above.
    updatePostDescriptors();

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
        // What makes the mip chain worth generating.
        //
        // Plain mip mapping picks a level from how fast the texture coordinates
        // change, and has to take the FASTER of the two axes or it aliases. On
        // a floor seen edge-on those two differ enormously - a screen pixel
        // covers a few texels across and dozens along - so the level that stops
        // the shimmer is far blurrier than the surface deserves, and the ground
        // turns to mush a few metres out. Anisotropic filtering takes several
        // samples along the stretched direction instead, which is the whole
        // difference between a game floor and a smear.
        .anisotropyEnable = m_device->maxAnisotropy() > 1.0f ? VK_TRUE : VK_FALSE,
        .maxAnisotropy = std::min(m_device->maxAnisotropy(), 16.0f),
        .minLod = 0.0f,
        // No ceiling: a texture may have as many levels as it has, and clamping
        // to zero here is exactly the bug that made every surface alias no
        // matter how many levels were generated for it.
        .maxLod = VK_LOD_CLAMP_NONE,
    });

    // The same sampler with the edges clamped instead of repeated.
    //
    // Every post pass reads NEIGHBOURING texels, so a pixel on the left edge of
    // the screen asks for texels past it - and with a repeating sampler what it
    // finds there is the right edge of the frame. A bright window on one side
    // would put a faint glow on the other, for no reason anyone could see in
    // the scene. Materials want repeat, because that is what tiling means; post
    // passes want clamp.
    m_clampSampler = m_device->handle().createSamplerUnique(vk::SamplerCreateInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eLinear,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .anisotropyEnable = VK_FALSE,
        .minLod = 0.0f,
        // Zero, not the chain length: every bloom view covers exactly one
        // level, so "level 0" of that view already means the level meant.
        .maxLod = 0.0f,
    });

    // For the depth buffer, which the temporal pass reads to turn a pixel back
    // into a point in the world.
    //
    // Nearest, and not as an approximation: linear filtering of a depth format
    // is OPTIONAL in Vulkan and most drivers do not offer it, so a linear
    // sampler here is a validation error rather than a soft result. It would
    // also be meaningless - halfway between the depth of a near surface and a
    // far one there is no surface at all, and reconstructing a position from
    // that average puts the point in mid-air.
    m_pointSampler = m_device->handle().createSamplerUnique(vk::SamplerCreateInfo{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .anisotropyEnable = VK_FALSE,
        .minLod = 0.0f,
        .maxLod = 0.0f,
    });
}

void Renderer::createDescriptors() {
    const vk::Device handle = m_device->handle();

    // Both stages: the vertex shader needs the matrices, the fragment shader
    // needs the sun and sky. A binding is only visible to the stages named here.
    rhi::DescriptorSetLayoutBuilder frameLayout;
    frameLayout.binding(0, vk::DescriptorType::eUniformBuffer,
                        vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment);

    // Only declared when the GPU can trace: the descriptor TYPE itself comes
    // from VK_KHR_acceleration_structure, so naming it without the extension
    // enabled is invalid. This is why there are two builds of the mesh fragment
    // shader rather than one that branches.
    if (m_device->rayTracingSupported()) {
        frameLayout.binding(1, vk::DescriptorType::eAccelerationStructureKHR,
                            vk::ShaderStageFlagBits::eFragment);
        frameLayout.binding(2, vk::DescriptorType::eStorageBuffer,
                            vk::ShaderStageFlagBits::eFragment);
        frameLayout.binding(3, vk::DescriptorType::eCombinedImageSampler,
                            vk::ShaderStageFlagBits::eFragment, kMaxSceneTextures);
    }
    m_cameraSetLayout = frameLayout.build(handle);

    m_materialSetLayout = rhi::DescriptorSetLayoutBuilder()
                              .binding(0, vk::DescriptorType::eCombinedImageSampler,
                                       vk::ShaderStageFlagBits::eFragment)
                              .build(handle);

    // The tone mapper reads the sharp scene and the glow and mixes them, so it
    // needs two bindings where every other fullscreen pass needs one.
    m_postSetLayout = rhi::DescriptorSetLayoutBuilder()
                          .binding(0, vk::DescriptorType::eCombinedImageSampler,
                                   vk::ShaderStageFlagBits::eFragment)
                          .binding(1, vk::DescriptorType::eCombinedImageSampler,
                                   vk::ShaderStageFlagBits::eFragment)
                          .build(handle);

    // The temporal pass reads three: the frame just rendered, the running
    // average of the ones before it, and the depth that says which pixel of the
    // second corresponds to which pixel of the first.
    m_temporalSetLayout = rhi::DescriptorSetLayoutBuilder()
                              .binding(0, vk::DescriptorType::eCombinedImageSampler,
                                       vk::ShaderStageFlagBits::eFragment)
                              .binding(1, vk::DescriptorType::eCombinedImageSampler,
                                       vk::ShaderStageFlagBits::eFragment)
                              .binding(2, vk::DescriptorType::eCombinedImageSampler,
                                       vk::ShaderStageFlagBits::eFragment)
                              .build(handle);

    // One camera set per frame in flight, plus one material set per loaded
    // material. Pools do not grow, so the material budget is fixed up front -
    // a real asset system would allocate a pool per scene instead of guessing.
    constexpr u32 kMaterialBudget = 64;

    // Plus the image sets the renderer keeps for itself, none of which belong
    // to a material: one per level of the bloom chain, so a pass can bind the
    // level below it as its source without rewriting a descriptor mid-frame,
    // and then two each for the tone mapper, the bloom chain's first source and
    // the temporal pass - two because the accumulated image alternates between
    // a pair, and a descriptor cannot be rewritten mid-flight.
    constexpr u32 kInternalImageSets = 6 + kBloomMips;

    // More descriptors than sets, because three of those sets hold more than
    // one image each: the tone mapper reads the scene and the glow, and the
    // temporal pass reads the scene, the history and the depth.
    constexpr u32 kInternalImageDescriptors = 2 * (2 + 1 + 3) + kBloomMips;

    std::vector<vk::DescriptorPoolSize> poolSizes{
        vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, rhi::kFramesInFlight},
        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler,
                               kMaterialBudget + kInternalImageDescriptors},
    };
    if (m_device->rayTracingSupported()) {
        poolSizes.push_back(vk::DescriptorPoolSize{vk::DescriptorType::eAccelerationStructureKHR,
                                                   rhi::kFramesInFlight});
        poolSizes.push_back(
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, rhi::kFramesInFlight});

        // Every slot of the array, for every frame in flight. This is the one
        // place the descriptor budget stops being trivial.
        poolSizes.push_back(vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler,
                                                   kMaxSceneTextures * rhi::kFramesInFlight});
    }
    m_descriptorPool = std::make_unique<rhi::DescriptorPool>(
        *m_device, rhi::kFramesInFlight + kMaterialBudget + kInternalImageSets, poolSizes);

    // The uniform buffers themselves outlive any number of scene loads; only
    // the descriptor sets pointing at them are reallocated.
    if (m_device->rayTracingSupported()) {
        for (PerFrame& frame : m_perFrame) {
            // Host visible and rewritten every frame, like the uniforms: the
            // list changes whenever anything in the scene moves.
            frame.instanceData = rhi::Buffer(*m_device,
                                             rhi::BufferDesc{
                                                 .size = sizeof(InstanceRecord) * kMaxInstances,
                                                 .usage = vk::BufferUsageFlagBits::eStorageBuffer,
                                                 .hostVisible = true,
                                             });
        }
    }

    for (PerFrame& frame : m_perFrame) {
        frame.cameraUniforms = rhi::Buffer(*m_device, rhi::BufferDesc{
                                                          .size = sizeof(FrameUniforms),
                                                          .usage = vk::BufferUsageFlagBits::eUniformBuffer,
                                                          // Rewritten every
                                                          // frame, so the CPU
                                                          // needs direct access.
                                                          .hostVisible = true,
                                                      });
    }

    allocateDescriptorSets();
}

void Renderer::allocateDescriptorSets() {
    rhi::DescriptorWriter writer;

    for (PerFrame& frame : m_perFrame) {
        frame.cameraSet = m_descriptorPool->allocate(*m_cameraSetLayout);
        writer.buffer(frame.cameraSet, 0, frame.cameraUniforms.handle(), sizeof(FrameUniforms));

        if (frame.instanceData.valid()) {
            writer.buffer(frame.cameraSet, 2, frame.instanceData.handle(),
                          frame.instanceData.size(), vk::DescriptorType::eStorageBuffer);
        }
    }

    // In pairs, because the image these name is one of the two the temporal
    // filter alternates between. Which one is current changes every frame, and
    // rewriting a descriptor a frame in flight may be reading is not allowed -
    // so both exist and the recording picks one.
    for (u32 slot = 0; slot < 2; ++slot) {
        m_tonemapSets[slot] = m_descriptorPool->allocate(*m_postSetLayout);
        m_bloomSourceSets[slot] = m_descriptorPool->allocate(*m_materialSetLayout);
        m_temporalSets[slot] = m_descriptorPool->allocate(*m_temporalSetLayout);
    }

    // One set per level of the bloom chain, allocated once here and rewritten
    // on a resize. Allocated once because the pool cannot free individual sets,
    // so allocating on every resize would drain it in a few drags of the panel
    // edge.
    m_bloomMipSets.resize(kBloomMips);
    for (vk::DescriptorSet& set : m_bloomMipSets) {
        set = m_descriptorPool->allocate(*m_materialSetLayout);
    }

    // Fills in all of the above. Everything they point at was created by
    // createViewportTarget, which runs before this.
    updatePostDescriptors();

    // The camera sets were just reallocated, so whatever was written into their
    // texture array went with them.
    m_textureArrayDirty = true;

    // The fallback material: used by anything with no material of its own, so
    // an untextured or broken asset still binds something valid.
    Material fallback;
    fallback.name = "default";
    fallback.baseColorFactor = Vec4{0.30f, 0.31f, 0.33f, 1.0f};
    fallback.descriptorSet = m_descriptorPool->allocate(*m_materialSetLayout);
    writer.image(fallback.descriptorSet, 0, m_defaultTexture.view(), *m_sampler);
    m_resources.setFallbackMaterial(m_resources.addMaterial(std::move(fallback)));

    writer.submit(m_device->handle());
}

namespace {

/// Lowercased extension, so ".GLB" and ".glb" are the same thing.
std::string lowerExtension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

} // namespace

void Renderer::refreshTextureArray() {
    if (!m_device->rayTracingSupported()) {
        return;
    }

    // The array lives in the per-frame set, and the other frame may still be
    // reading it. This only runs after something added a texture - loading a
    // model, creating a material - so the stall lands where the loading already
    // was, not in the middle of a frame.
    m_device->waitIdle();

    rhi::DescriptorWriter writer;
    const u32 count = static_cast<u32>(m_resources.textureCount());

    for (PerFrame& frame : m_perFrame) {
        for (u32 slot = 0; slot < kMaxSceneTextures; ++slot) {
            // Past the end of the scene, the default texture. Every slot is
            // written, so an index that is somehow stale samples white instead
            // of reading a descriptor that was never filled in - which is
            // undefined behaviour rather than a visible mistake.
            const vk::ImageView view = slot < count
                                           ? m_resources.texture(TextureHandle{slot}).view()
                                           : m_defaultTexture.view();

            writer.image(frame.cameraSet, 3, view, *m_sampler,
                         vk::ImageLayout::eShaderReadOnlyOptimal, slot);
        }
    }

    writer.submit(m_device->handle());
    m_textureArrayDirty = false;

    FUMAR_DEBUG("texture array refreshed, {} texture(s) in the scene", count);
}

void Renderer::updatePostDescriptors() {
    rhi::DescriptorWriter writer;

    // The clamped sampler, not the repeating one the materials use: every one
    // of these is read by a pass that reaches past the edge of the image, and
    // what a repeating sampler finds there is the opposite edge of the frame.
    for (u32 slot = 0; slot < 2; ++slot) {
        // `slot` is which accumulated image is CURRENT. The tone mapper and the
        // bloom chain read that one; the temporal pass reads the other, which is
        // last frame's, and writes this one.
        const u32 history = slot ^ 1u;

        writer.image(m_tonemapSets[slot], 0, m_accum[slot].view(), *m_clampSampler);
        writer.image(m_tonemapSets[slot], 1, *m_bloomMipViews[0], *m_clampSampler);

        writer.image(m_bloomSourceSets[slot], 0, m_accum[slot].view(), *m_clampSampler);

        writer.image(m_temporalSets[slot], 0, m_sceneHdr.view(), *m_clampSampler);
        writer.image(m_temporalSets[slot], 1, m_accum[history].view(), *m_clampSampler);
        writer.image(m_temporalSets[slot], 2, sampledDepth().view(), *m_pointSampler);
    }

    for (u32 level = 0; level < m_bloomMipCount; ++level) {
        writer.image(m_bloomMipSets[level], 0, *m_bloomMipViews[level], *m_clampSampler);
    }

    writer.submit(m_device->handle());
}

bool Renderer::isImportable(const std::filesystem::path& path) {
    const std::string extension = lowerExtension(path);
    return extension == ".gltf" || extension == ".glb" || extension == ".png" ||
           extension == ".jpg" || extension == ".jpeg" || extension == ".tga" ||
           extension == ".bmp";
}

NodeId Renderer::importAsset(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        FUMAR_WARN("cannot import '{}': no such file", path.string());
        return kInvalidNode;
    }

    const std::string extension = lowerExtension(path);

    if (extension == ".gltf" || extension == ".glb") {
        const NodeId root = loadModel(path);
        if (root != kInvalidNode) {
            // Dropped in front of the camera rather than at the origin, so it
            // lands where you were looking.
            m_scene.node(root).transform.position = m_camera.position + m_camera.forward() * 6.0f;
        }
        return root;
    }

    // An image is a material, not geometry: there is nothing to place, but
    // there is now something to assign in the Details panel.
    const MaterialHandle material =
        createMaterial(path.stem().string(), Vec4{1.0f, 1.0f, 1.0f, 1.0f}, path);
    FUMAR_INFO("imported texture '{}' as material #{}", path.filename().string(), material.index);
    return kInvalidNode;
}

void Renderer::resetScene() {
    // Anything about to be released may still be referenced by a frame the GPU
    // has not finished.
    m_device->waitIdle();

    m_selected = kInvalidNode;
    m_highlighted = kInvalidNode;
    m_isolated = kInvalidNode;

    m_scene = Scene{};
    m_resources.clear();
    m_retiredMeshes.clear();

    // The instance list still points at bottom level structures belonging to
    // the meshes just released. Nothing traces against them before the next
    // frame rebuilds it, but a stale handle already written into a descriptor
    // would outlive them, so both are dropped here.
    for (PerFrame& frame : m_perFrame) {
        frame.topLevel = rhi::TopLevelStructure{};
        frame.writtenStructure = nullptr;
    }

    // Frees every descriptor set at once - including the camera sets, which is
    // why they are reallocated immediately afterwards.
    m_descriptorPool->reset();
    allocateDescriptorSets();
}

MeshHandle Renderer::createCubeMesh() {
    return createMesh(geometry::makeCube(), proceduralMesh("cube"));
}

MeshHandle Renderer::createPlaneMesh(f32 halfSize, f32 uvTiling) {
    return createMesh(geometry::makePlane(halfSize, uvTiling),
                      proceduralMesh("plane", Vec4{halfSize, uvTiling, 0.0f, 0.0f}));
}

MeshHandle Renderer::createCylinderMesh(f32 radius, f32 height, u32 segments) {
    return createMesh(geometry::makeCylinder(radius, height, segments),
                      proceduralMesh("cylinder", Vec4{radius, height, static_cast<f32>(segments), 0.0f}));
}

MeshHandle Renderer::createSphereMesh(f32 radius, u32 segments, u32 rings) {
    return createMesh(geometry::makeUvSphere(radius, segments, rings),
                      proceduralMesh("sphere", Vec4{radius, static_cast<f32>(segments),
                                                    static_cast<f32>(rings), 0.0f}));
}

MeshHandle Renderer::createMesh(const geometry::EditableMesh& mesh, MeshSource source) {
    return m_resources.addMesh(makeMesh(*m_device, *m_upload, mesh), std::move(source));
}

void Renderer::updateMesh(MeshHandle handle, const geometry::EditableMesh& mesh) {
    if (!m_resources.has(handle)) {
        return;
    }

    // Built first and swapped second, so there is never a moment when the
    // handle points at nothing. The upload context waits for its own copy to
    // finish, so the new buffers are complete by the time they are swapped in.
    Mesh replacement = makeMesh(*m_device, *m_upload, mesh);
    m_retiredMeshes.push_back(RetiredMesh{
        .frame = m_frameCounter,
        .mesh = m_resources.replaceMesh(handle, std::move(replacement)),
    });
}

void Renderer::releaseRetiredMeshes() {
    // A mesh retired when the counter read N may be referenced by frames up to
    // N - 1. Each frame waits for the one kFramesInFlight before it, so by the
    // time the counter reaches N + kFramesInFlight, all of those are done.
    std::erase_if(m_retiredMeshes, [this](const RetiredMesh& retired) {
        return retired.frame + rhi::kFramesInFlight <= m_frameCounter;
    });
}

template <typename Function>
void Renderer::forEachRenderedDrawable(Function&& function) const {
    m_scene.forEachDrawable([&](NodeId id, const Node& node, const Mat4& worldTransform) {
        if (m_isolated != kInvalidNode && id != m_isolated) {
            return;
        }
        function(id, node, worldTransform);
    });
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
            m_textureArrayDirty = true;
            material.baseColorPath = baseColorTexture.string();
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

    // A glTF brings its own textures, however many; the array has to be
    // rewritten before a ray can reach any of them.
    m_textureArrayDirty = true;

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

f32 Renderer::raycast(const Ray& ray, f32 maxDistance) const {
    f32 nearest = maxDistance;
    bool hit = false;

    m_scene.forEachDrawable([&](NodeId, const Node& node, const Mat4& worldTransform) {
        if (!m_resources.has(node.mesh)) {
            return;
        }

        // Into the object own space, where its bounding box really is axis
        // aligned. The direction is deliberately left unnormalised after the
        // transform: scaling has to affect it for the distance that comes back
        // to be in world units.
        const Mat4 toLocal = inverse(worldTransform);
        const Vec3 localOrigin = xyz(toLocal * point(ray.origin));
        const Vec3 localDirection = xyz(toLocal * direction(ray.direction));

        f32 distance = 0.0f;
        if (!intersectRayBounds(localOrigin, localDirection, m_resources.mesh(node.mesh).bounds(),
                                distance)) {
            return;
        }

        if (distance >= 0.0f && distance < nearest) {
            nearest = distance;
            hit = true;
        }
    });

    return hit ? nearest : -1.0f;
}

NodeId Renderer::pickNode(const Ray& ray) const {
    NodeId best = kInvalidNode;
    f32 bestDistance = 1e30f;

    // Lights have no geometry, so a bounding box test would never reach them.
    // They are tested against a small sphere around the marker instead - which
    // is exactly the thing on screen the user is aiming at.
    m_scene.traverse([&](NodeId id, u32) {
        const Node& node = m_scene.node(id);
        if (!node.light.has_value() || !node.visible) {
            return;
        }

        const Mat4& world = m_scene.worldTransform(id);
        const Vec3 centre{world.columns[3].x, world.columns[3].y, world.columns[3].z};

        // Ray-sphere, solved by dropping a perpendicular: the closest approach
        // of the ray to the centre. Cheaper than the quadratic and enough here,
        // because the exact entry point does not matter - only which light is
        // nearest and whether the ray came close enough to count.
        constexpr f32 kMarkerRadius = 0.42f;
        const Vec3 toCentre = centre - ray.origin;
        const f32 along = dot(toCentre, ray.direction);
        if (along < 0.0f) {
            return;
        }
        const Vec3 closest = ray.origin + ray.direction * along;
        if (lengthSquared(centre - closest) > kMarkerRadius * kMarkerRadius) {
            return;
        }

        if (along < bestDistance) {
            bestDistance = along;
            best = id;
        }
    });

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

bool Renderer::rayTracingSupported() const {
    return m_device->rayTracingSupported();
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

void Renderer::updateFrameUniforms(u32 frameIndex) {
    // The aspect ratio of the image being rendered INTO, which is the viewport
    // panel - not the window. Using the window's would stretch everything by
    // however much the panel differs from it, and since the panel is docked
    // beside other panels, it always does.
    const f32 aspect = static_cast<f32>(m_viewportExtent.width) /
                       static_cast<f32>(m_viewportExtent.height);

    const Mat4 view = m_camera.view();
    const Mat4 projection = m_camera.projection(aspect);
    const Environment& env = m_environment;

    FrameUniforms uniforms{
        .view = view,
        .projection = projection,
        // Inverted here rather than in the shader: it is the same matrix for
        // every one of the two million pixels the sky covers, so computing it
        // once per frame on the CPU is free by comparison.
        .invViewProjection = inverse(projection * view),
        .prevViewProjection = m_prevViewProjection,
        .cameraPosition = point(m_camera.position),
        .prevCameraPosition = point(m_prevCameraPosition),
        .sunDirection = direction(env.sunDirection()),
        .sunColor = Vec4{env.sunColor.x, env.sunColor.y, env.sunColor.z, 1.0f},
        .skyZenithColor = Vec4{env.skyZenithColor.x, env.skyZenithColor.y, env.skyZenithColor.z, 1.0f},
        .skyHorizonColor = Vec4{env.skyHorizonColor.x, env.skyHorizonColor.y, env.skyHorizonColor.z, 1.0f},
        .groundColor = Vec4{env.groundColor.x, env.groundColor.y, env.groundColor.z, 1.0f},
        .sunIntensity = env.sunIntensity,
        .sunAngularRadius = radians(env.sunAngularRadiusDegrees),
        .skyIntensity = env.skyIntensity,
        .exposure = env.exposure,
        // Zeroed rather than branched on in the shader when the GPU cannot
        // trace: the ray tracing functions are already compiled out there, and
        // leaving a live value in the buffer would be misleading to read back.
        .shadowStrength = m_device->rayTracingSupported() ? env.shadowStrength : 0.0f,
        .occlusionStrength = m_device->rayTracingSupported() ? env.occlusionStrength : 0.0f,
        .occlusionRadius = env.occlusionRadius,
        .reflectionStrength = m_device->rayTracingSupported() ? env.reflectionStrength : 0.0f,
        .reflectionRoughnessLimit = env.reflectionRoughnessLimit,
        .indirectStrength =
            m_device->rayTracingSupported() ? env.indirectStrength : 0.0f,
        .bloomStrength = env.bloomStrength,
        .bloomThreshold = env.bloomThreshold,
        // Zero on the frame after the images were created, because there is no
        // history to carry over yet - the pair holds uninitialised memory, and
        // averaging that in would be a flash of whatever was in it. Zero means
        // "this frame only", which is exactly right once.
        //
        // Also zero when the GPU cannot trace: with no rays there is no
        // sampling noise, and nothing left for the filter to do but add lag.
        .temporalStrength = (m_historyValid && m_device->rayTracingSupported())
                                ? env.temporalStrength
                                : 0.0f,
        // Turns the sampling patterns, so each frame traces different rays and
        // the average over frames is an average of different estimates. Held at
        // zero when the filter is off, which locks the pattern to the geometry -
        // grain that sits still looks like texture, grain that moves looks like
        // a fault.
        .frameIndex = (m_historyValid && env.temporalStrength > 0.0f)
                          ? static_cast<i32>(m_frameCounter % kSamplePatternPeriod)
                          : 0,
        // Filled in below, once the scene has been walked.
        .lightCount = 0,
        .lights = {},
    };

    // --- lights ---------------------------------------------------------
    // Gathered from the scene every frame rather than kept in a list that has
    // to be maintained: a light is a node, and nodes are created, deleted,
    // hidden and reparented by machinery that knows nothing about lighting.
    // Walking the tree is the only way to stay right without that machinery
    // having to tell anyone.
    //
    // None at all while an object is isolated: the Modeler shows it under the
    // sky alone, and a lamp somewhere in the hidden scene lighting it from
    // nowhere visible would only be confusing.
    u32 lightCount = 0;
    m_scene.traverse([&](NodeId id, u32) {
        if (lightCount >= kMaxLights || m_isolated != kInvalidNode) {
            return;
        }
        const Node& node = m_scene.node(id);
        if (!node.light.has_value() || !node.visible) {
            return;
        }

        const Light& light = *node.light;
        const Mat4& world = m_scene.worldTransform(id);

        // The last column of a transform is where it puts the origin, which is
        // the light's position. No separate field for it, so a lamp parented to
        // a moving object follows it for free.
        const Vec3 position{world.columns[3].x, world.columns[3].y, world.columns[3].z};

        // -Z is forward, the glTF and OpenGL convention fumar follows, so a
        // spot light points where its node points.
        const Vec3 direction =
            normalize(Vec3{-world.columns[2].x, -world.columns[2].y, -world.columns[2].z});

        const bool spot = light.type == LightType::Spot;
        uniforms.lights[lightCount] = LightUniform{
            .positionRange = Vec4{position.x, position.y, position.z, light.range},
            .colorIntensity =
                Vec4{light.color.x, light.color.y, light.color.z, light.intensity},
            .directionOuter = Vec4{direction.x, direction.y, direction.z,
                                   std::cos(radians(light.outerConeDegrees))},
            .shape = Vec4{std::cos(radians(light.innerConeDegrees)), light.sourceRadius,
                          spot ? 1.0f : 0.0f,
                          // Shadows are only traced when the hardware can trace
                          // at all; otherwise the ray functions are compiled out
                          // and this would be a promise nothing keeps.
                          (light.castsShadows && m_device->rayTracingSupported()) ? 1.0f : 0.0f},
        };
        ++lightCount;
    });

    uniforms.lightCount = static_cast<i32>(lightCount);

    // A plain memcpy into persistently mapped memory. No fence is needed: this
    // slot's previous frame was already waited on before we got here.
    m_perFrame[frameIndex].cameraUniforms.write(&uniforms, sizeof(uniforms));

    // Last, so that everything above read the PREVIOUS values. This is the
    // whole of the temporal filter's state on the CPU side: where the camera
    // was when the picture it is about to reuse was taken.
    m_prevViewProjection = projection * view;
    m_prevCameraPosition = m_camera.position;
    ++m_frameCounter;
}

void Renderer::recordAccelerationStructure(vk::CommandBuffer cmd, u32 frameIndex) {
    if (!m_device->rayTracingSupported()) {
        return;
    }

    PerFrame& frame = m_perFrame[frameIndex];

    // One entry per drawable: which mesh, and where it is. The bottom level
    // structures hold the triangles; this holds nothing but a matrix and a
    // pointer to one, which is why rebuilding it every frame is affordable and
    // rebuilding the meshes would not be.
    m_instances.clear();
    m_instanceRecords.clear();

    forEachRenderedDrawable([&](NodeId, const Node& node, const Mat4& worldTransform) {
        if (!m_resources.has(node.mesh) || m_instances.size() >= kMaxInstances) {
            return;
        }
        const Mesh& mesh = m_resources.mesh(node.mesh);
        if (!mesh.accelerationStructure().valid()) {
            return;
        }

        vk::AccelerationStructureInstanceKHR instance{};
        instance.transform = rhi::toTransformMatrix(&worldTransform.columns[0].x);

        // Hit by every ray whose mask overlaps this one. A single mask is
        // enough for now; separate bits would let shadow rays ignore something
        // that camera rays still see, glass being the usual example.
        instance.mask = 0xFF;

        // The one number a ray gets back that means anything to us. Set to this
        // instance's position in the record list, so a hit can look up what it
        // hit - the alternative is a ray that knows a triangle was there and
        // nothing whatsoever about it.
        instance.instanceCustomIndex = static_cast<u32>(m_instanceRecords.size());

        instance.flags =
            static_cast<u32>(vk::GeometryInstanceFlagBitsKHR::eTriangleFacingCullDisable);
        instance.accelerationStructureReference = mesh.accelerationStructure().deviceAddress();

        const MaterialHandle handle =
            m_resources.has(node.material) ? node.material : m_resources.fallbackMaterial();
        const Material& material = m_resources.has(handle) ? m_resources.material(handle)
                                                           : Material{};

        m_instanceRecords.push_back(InstanceRecord{
            .vertices = mesh.vertexBuffer().deviceAddress(),
            .indices = mesh.indexBuffer().deviceAddress(),
            .baseColor = material.baseColorFactor,
            .metallic = material.metallic,
            .roughness = material.roughness,
            .texture = m_resources.has(material.baseColor) &&
                               material.baseColor.index < kMaxSceneTextures
                           ? material.baseColor.index
                           : kNoTexture,
            .uvScale = material.uvScale,
            .padding = 0.0f,
        });

        m_instances.push_back(instance);
    });

    if (!m_instanceRecords.empty() && frame.instanceData.valid()) {
        frame.instanceData.write(m_instanceRecords.data(),
                                 m_instanceRecords.size() * sizeof(InstanceRecord));
    }

    frame.topLevel.record(*m_device, cmd, m_instances);

    // Reallocating the structure invalidates the descriptor pointing at it.
    // record() waits for the device before it does that, so writing here is
    // safe - and it only happens when the handle actually changed.
    if (frame.topLevel.handle() != frame.writtenStructure) {
        rhi::DescriptorWriter writer;
        writer.accelerationStructure(frame.cameraSet, 1, frame.topLevel.handle());
        writer.submit(m_device->handle());
        frame.writtenStructure = frame.topLevel.handle();
    }
}

void Renderer::recordSceneRendering(vk::CommandBuffer cmd) {
    const vk::Extent2D extent{m_viewportExtent.width, m_viewportExtent.height};

    // --- barriers: both attachments into writable layouts -------------------
    const std::array<vk::ImageMemoryBarrier2, 2> toAttachment{
        vk::ImageMemoryBarrier2{
            // Waits for the fragment shader that sampled this image last frame,
            // when the tone mapper read it. Overwriting it before that read
            // completes is exactly the hazard this barrier exists for.
            .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            // The resolve target, whether or not anything is resolved into
            // it: with one sample the geometry lands here directly.
            .image = m_sceneHdr.handle(),
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

    if (m_sceneHdrMs.valid()) {
        const std::array<vk::ImageMemoryBarrier2, 2> msToAttachment{
            // Nothing to wait on and nothing to preserve: this image exists
            // only between the first draw of the pass and the resolve at the
            // end of it, and never leaves the pass at all.
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = m_sceneHdrMs.handle(),
                .subresourceRange = kWholeColorImage,
            },
            // Where depth is resolved to, which the temporal pass read last
            // frame - hence the wait on a fragment shader rather than on
            // anything to do with depth.
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                vk::PipelineStageFlagBits2::eLateFragmentTests,
                .dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = m_depthResolved.handle(),
                .subresourceRange = kWholeDepthImage,
            },
        };

        cmd.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = static_cast<u32>(msToAttachment.size()),
            .pImageMemoryBarriers = msToAttachment.data(),
        });
    }

    // Black rather than a background colour: the sky pass below covers every
    // pixel, so this is only ever seen if that pass fails - and a black frame
    // says so much more clearly than a plausible-looking grey one.
    vk::ClearValue colorClear{};
    colorClear.color.float32[3] = 1.0f;

    vk::ClearValue depthClear{};
    // 1.0 is the far plane in Vulkan 0..1 depth, so clearing to it means
    // nothing has been drawn here yet and any geometry passes the eLess test.
    depthClear.depthStencil.depth = 1.0f;

    // With anti-aliasing on, the draws go into the multisampled image and the
    // hardware averages the samples into m_sceneHdr when the pass ends. The
    // resolve is free in the sense that matters: it happens as the tiles are
    // written out, so the multisampled data need never make the round trip to
    // memory that doing it by hand would force.
    const bool multisampled = m_sceneHdrMs.valid();

    const vk::RenderingAttachmentInfo colorAttachment{
        .imageView = multisampled ? m_sceneHdrMs.view() : m_sceneHdr.view(),
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .resolveMode = multisampled ? vk::ResolveModeFlagBits::eAverage
                                    : vk::ResolveModeFlagBits::eNone,
        .resolveImageView = multisampled ? m_sceneHdr.view() : vk::ImageView{},
        .resolveImageLayout = multisampled ? vk::ImageLayout::eColorAttachmentOptimal
                                           : vk::ImageLayout::eUndefined,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = colorClear,
    };

    // Depth is resolved as well as colour, because the temporal pass needs it:
    // depth is the only thing that can turn a pixel back into a point in the
    // world, which is what makes reprojecting the previous frame possible.
    //
    // eSampleZero, not eAverage. It is the one depth resolve mode Vulkan
    // requires every implementation to support, and it is also the correct one
    // - averaging the depths of a near surface and a far one gives a distance
    // at which there is no surface at all.
    const vk::RenderingAttachmentInfo depthAttachment{
        .imageView = m_depthImage.view(),
        .imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
        .resolveMode = multisampled ? vk::ResolveModeFlagBits::eSampleZero
                                    : vk::ResolveModeFlagBits::eNone,
        .resolveImageView = multisampled ? m_depthResolved.view() : vk::ImageView{},
        .resolveImageLayout = multisampled ? vk::ImageLayout::eDepthAttachmentOptimal
                                           : vk::ImageLayout::eUndefined,
        .loadOp = vk::AttachmentLoadOp::eClear,
        // With anti-aliasing on, the multisampled depth really is scratch space
        // for this pass - the resolve happens as the pass ends regardless of
        // the store, so saying it is not needed lets the driver skip writing
        // several samples per pixel out to memory. Without anti-aliasing there
        // is nothing to resolve INTO and this image is what gets sampled, so it
        // has to survive the pass.
        .storeOp = multisampled ? vk::AttachmentStoreOp::eDontCare
                                : vk::AttachmentStoreOp::eStore,
        .clearValue = depthClear,
    };

    cmd.beginRendering(vk::RenderingInfo{
        .renderArea = vk::Rect2D{.offset = {0, 0}, .extent = extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachment,
        .pDepthAttachment = &depthAttachment,
    });

    const vk::Viewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = static_cast<f32>(extent.width),
        .height = static_cast<f32>(extent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    const vk::Rect2D scissor{.offset = {0, 0}, .extent = extent};
    const vk::DescriptorSet frameSet = m_perFrame[m_frames->currentFrame()].cameraSet;

    // --- sky ----------------------------------------------------------------
    // First, so everything drawn afterwards has something to sit against. It
    // writes no depth, so geometry is not rejected by it; and because it covers
    // every pixel there is nothing for the colour clear above to do.
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_skyPipeline->handle());
    cmd.setViewport(0, viewport);
    cmd.setScissor(0, scissor);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_skyPipeline->layout(), 0, frameSet, {});
    if (m_resources.has(m_resources.fallbackMaterial())) {
        // Set 1 is unused by the sky shader, but the layout declares it, so
        // something compatible has to be bound before the draw is legal.
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_skyPipeline->layout(), 1,
                               m_resources.material(m_resources.fallbackMaterial()).descriptorSet, {});
    }

    // Three vertices, no buffer: fullscreen.vert builds them from gl_VertexIndex.
    cmd.draw(3, 1, 0, 0);

    // --- geometry -----------------------------------------------------------
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_pipeline->handle());

    cmd.setViewport(0, viewport);
    cmd.setScissor(0, scissor);

    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_pipeline->layout(), 0, frameSet, {});

    // Materials are bound per drawable rather than once, because different
    // nodes use different ones. Consecutive nodes usually share a material
    // though, so the last one bound is remembered and the rebind skipped -
    // sorting the scene by material would take this further.
    vk::DescriptorSet boundMaterial;

    forEachRenderedDrawable([&](NodeId id, const Node& node, const Mat4& worldTransform) {
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
            .uvScale = material.uvScale,
            .metallic = material.metallic,
            .roughness = material.roughness,
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
            .uvScale = Vec2{1.0f, 1.0f},
            .metallic = 0.0f,
            .roughness = 1.0f,
            .highlight = strength,
        };
        cmd.pushConstants<ObjectPushConstants>(
            m_outlinePipeline->layout(),
            vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, push);

        m_resources.mesh(node.mesh).draw(cmd);
    };

    // Lights first, so an outline drawn over one still wins.
    const bool anyLight = m_scene.nodeCount() > 0 && m_lightMarker.valid() && m_isolated == kInvalidNode;
    if (anyLight) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_outlinePipeline->handle());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_outlinePipeline->layout(), 0,
                               frameSet, {});
        if (m_resources.has(m_resources.fallbackMaterial())) {
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_outlinePipeline->layout(), 1,
                                   m_resources.material(m_resources.fallbackMaterial()).descriptorSet,
                                   {});
        }

        m_scene.traverse([&](NodeId id, u32) {
            const Node& node = m_scene.node(id);
            if (!node.light.has_value() || !node.visible) {
                return;
            }

            const Mat4& world = m_scene.worldTransform(id);
            const Vec3 position{world.columns[3].x, world.columns[3].y, world.columns[3].z};

            // Built from the position alone rather than from the node's own
            // matrix, so a light parented under something scaled does not get a
            // stretched marker. A spot is drawn longer along its axis, which is
            // the only cheap way to see which way it points.
            Mat4 markerTransform = translation(position);
            if (node.light->type == LightType::Spot) {
                // The rotation columns, normalised to strip any inherited scale.
                const Vec3 axisX = normalize(xyz(world.columns[0]));
                const Vec3 axisY = normalize(xyz(world.columns[1]));
                const Vec3 axisZ = normalize(xyz(world.columns[2]));
                Mat4 orientation = identity();
                orientation.columns[0] = direction(axisX * 0.22f);
                orientation.columns[1] = direction(axisY * 0.22f);
                orientation.columns[2] = direction(axisZ * 0.55f);
                markerTransform = markerTransform * orientation;
            } else {
                markerTransform = markerTransform * scaling(Vec3{0.28f, 0.28f, 0.28f});
            }

            // Negative highlight tells outline.frag this is a marker and to
            // take its colour from baseColor - the light's own colour, so a
            // blue lamp reads as a blue lamp in the viewport.
            const Vec3 tint = node.light->color;
            const f32 boost = id == m_selected ? 4.0f : 2.2f;
            const ObjectPushConstants push{
                .model = markerTransform,
                .baseColor = Vec4{tint.x * boost, tint.y * boost, tint.z * boost, 1.0f},
                .uvScale = Vec2{1.0f, 1.0f},
                .metallic = 0.0f,
                .roughness = 1.0f,
                .highlight = -1.0f,
            };
            cmd.pushConstants<ObjectPushConstants>(
                m_outlinePipeline->layout(),
                vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, push);

            m_lightMarker.draw(cmd);
        });
    }

    if (m_highlighted != kInvalidNode || m_selected != kInvalidNode) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_outlinePipeline->handle());

        // The outline shader ignores both sets, but the layout still declares
        // them, so something compatible has to be bound or the draw is invalid.
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_outlinePipeline->layout(), 0,
                               frameSet, {});
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
}

void Renderer::recordTemporal(vk::CommandBuffer cmd) {
    const vk::Extent2D extent{m_viewportExtent.width, m_viewportExtent.height};

    // The pair swaps every frame: what was written last frame is now the
    // history being read, and what was read then is about to be overwritten.
    // Nothing outside this function needs to know which is which beyond "the
    // current one", which is what m_accumIndex names.
    m_accumIndex ^= 1u;
    const u32 current = m_accumIndex;
    const u32 history = current ^ 1u;

    const std::array<vk::ImageMemoryBarrier2, 4> entry{
        // The frame that was just rendered, about to be read as a texture.
        // eColorAttachmentOptimal as the old layout, not eUndefined: its
        // contents are the entire input.
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_sceneHdr.handle(),
            .subresourceRange = kWholeColorImage,
        },
        // Depth, likewise. Written by the depth test rather than by a shader,
        // so the stages it is waited on at are the fragment test ones.
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                            vk::PipelineStageFlagBits2::eLateFragmentTests,
            .srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eDepthAttachmentOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = sampledDepth().handle(),
            .subresourceRange = kWholeDepthImage,
        },
        // Last frame's average, which is already readable: the bloom chain put
        // it in this layout last frame and nothing has moved it since. So this
        // transitions nothing in the ordinary case, and exists for the first
        // frame after the images are created, when the layout really is
        // undefined - the shader names this binding, and the validation layers
        // check the layout of everything a shader COULD read, not of what it
        // does read with the history switched off.
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = m_historyValid ? vk::ImageLayout::eShaderReadOnlyOptimal
                                        : vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_accum[history].handle(),
            .subresourceRange = kWholeColorImage,
        },
        // Where this frame's average goes. Every pixel is written by the
        // fullscreen draw, so nothing is preserved; the wait is on the bloom
        // chain and the tone mapper, which read this same image two frames ago.
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_accum[current].handle(),
            .subresourceRange = kWholeColorImage,
        },
    };

    cmd.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(entry.size()),
        .pImageMemoryBarriers = entry.data(),
    });

    const vk::RenderingAttachmentInfo colorAttachment{
        .imageView = m_accum[current].view(),
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eDontCare,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };

    cmd.beginRendering(vk::RenderingInfo{
        .renderArea = vk::Rect2D{.offset = {0, 0}, .extent = extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachment,
    });

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_temporalPipeline->handle());
    cmd.setViewport(0, vk::Viewport{
                           .x = 0.0f,
                           .y = 0.0f,
                           .width = static_cast<f32>(extent.width),
                           .height = static_cast<f32>(extent.height),
                           .minDepth = 0.0f,
                           .maxDepth = 1.0f,
                       });
    cmd.setScissor(0, vk::Rect2D{.offset = {0, 0}, .extent = extent});

    // Set 0 for the camera matrices - this frame's and last frame's, which is
    // what the reprojection is - and set 1 for the three images.
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_temporalPipeline->layout(), 0,
                           m_perFrame[m_frames->currentFrame()].cameraSet, {});
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_temporalPipeline->layout(), 1,
                           m_temporalSets[current], {});

    cmd.draw(3, 1, 0, 0);
    cmd.endRendering();

    // There is now something in the pair worth reading next frame. Left to the
    // end so that a frame which never got this far - the first one after a
    // resize - cannot leave the flag set with nothing behind it.
    m_historyValid = true;
}

void Renderer::recordBloom(vk::CommandBuffer cmd) {
    // One level of the chain, as a subresource range - every barrier below
    // moves exactly one, because the levels are in different layouts at
    // different moments.
    const auto mipRange = [](u32 level) {
        return vk::ImageSubresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = level,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
    };

    // What the pass is about to read, and what it is about to overwrite.
    //
    // The whole bloom image goes to eUndefined rather than its previous layout
    // on purpose: every texel of every level is written before it is read, so
    // there is nothing in there worth keeping from last frame, and eUndefined
    // is what tells the driver it may throw the old contents away instead of
    // preserving them through the transition.
    const std::array<vk::ImageMemoryBarrier2, 2> entry{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            // eColorAttachmentOptimal, not eUndefined: the contents are the
            // whole point here. eUndefined tells the driver it may throw the
            // pixels away, which is right for an image about to be overwritten
            // and catastrophic for one about to be read.
            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            // The temporally accumulated scene, not the raw one: the bloom and
            // the tone mapper both work from what the temporal pass produced,
            // which is the same picture with the sampling noise averaged out.
            .image = m_accum[m_accumIndex].handle(),
            .subresourceRange = kWholeColorImage,
        },
        vk::ImageMemoryBarrier2{
            // Waits on last frame's reads of this image - the tone mapper's,
            // and the chain's own. Nothing is being preserved, so this is an
            // ordering constraint and not a memory one.
            .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_bloom.handle(),
            .subresourceRange =
                vk::ImageSubresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = m_bloomMipCount,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
        },
    };

    cmd.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(entry.size()),
        .pImageMemoryBarriers = entry.data(),
    });

    // Draws one fullscreen triangle into one level of the chain.
    const auto blit = [&](u32 level, vk::ImageView target, vk::Extent2D extent,
                          vk::AttachmentLoadOp loadOp) {
        const vk::RenderingAttachmentInfo attachment{
            .imageView = target,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = loadOp,
            .storeOp = vk::AttachmentStoreOp::eStore,
        };

        cmd.beginRendering(vk::RenderingInfo{
            .renderArea = vk::Rect2D{.offset = {0, 0}, .extent = extent},
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &attachment,
        });

        cmd.setViewport(0, vk::Viewport{
                               .x = 0.0f,
                               .y = 0.0f,
                               .width = static_cast<f32>(extent.width),
                               .height = static_cast<f32>(extent.height),
                               .minDepth = 0.0f,
                               .maxDepth = 1.0f,
                           });
        cmd.setScissor(0, vk::Rect2D{.offset = {0, 0}, .extent = extent});
        cmd.draw(3, 1, 0, 0);
        cmd.endRendering();

        // Written; now readable, because the next step of the chain reads
        // exactly what this one just wrote. This is the dependency that makes
        // the chain a chain.
        const vk::ImageMemoryBarrier2 toRead{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_bloom.handle(),
            .subresourceRange = mipRange(level),
        };

        cmd.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &toRead,
        });
    };

    // --- down ---------------------------------------------------------------
    // Level 0 reads the scene and keeps only what is bright; every level after
    // it reads the level above and merely blurs. Only the first step filters,
    // because once the bright pass has been extracted, filtering again would
    // eat into the glow it just produced.
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_bloomDownPipeline->handle());

    for (u32 level = 0; level < m_bloomMipCount; ++level) {
        const bool fromScene = level == 0;
        const vk::Extent2D source =
            fromScene ? vk::Extent2D{m_viewportExtent.width, m_viewportExtent.height}
                      : bloomMipExtent(level - 1);

        // m_bloomSourceSets rather than the tone mapper's, even though both
        // point at the same image: a bound set has to match the layout the
        // pipeline was built with, and the tone mapper's holds two bindings.
        cmd.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics, m_bloomDownPipeline->layout(), 0,
            fromScene ? m_bloomSourceSets[m_accumIndex] : m_bloomMipSets[level - 1], {});

        const BloomDownPush push{
            // Of the SOURCE, not the target: the filter is a pattern of taps
            // into the image being read.
            .texelSize = Vec2{1.0f / static_cast<f32>(source.width),
                              1.0f / static_cast<f32>(source.height)},
            .threshold = m_environment.bloomThreshold,
            // A shoulder just over half the threshold, rather than a number of
            // its own: a knee that does not scale with the threshold is either
            // a hard edge at high thresholds or the whole range at low ones.
            .knee = m_environment.bloomThreshold * 0.6f,
            .prefilter = fromScene ? 1 : 0,
        };

        cmd.pushConstants<BloomDownPush>(m_bloomDownPipeline->layout(),
                                         vk::ShaderStageFlagBits::eFragment, 0, push);

        blit(level, *m_bloomMipViews[level], bloomMipExtent(level), vk::AttachmentLoadOp::eDontCare);
    }

    // --- and back up --------------------------------------------------------
    // Each level is added into the one above it, so what reaches level 0 is the
    // sum of every level: the small ones spread wide and faint, the large ones
    // stay tight and bright. One blur at one radius cannot be both.
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_bloomUpPipeline->handle());

    for (u32 level = m_bloomMipCount; level-- > 1;) {
        const u32 target = level - 1;
        const vk::Extent2D source = bloomMipExtent(level);

        // The target was left readable by the downsample pass, and has to go
        // back to being an attachment. eShaderReadOnlyOptimal as the old layout
        // rather than eUndefined: what it holds is half the answer, and the
        // blend below adds to it.
        const vk::ImageMemoryBarrier2 toAttachment{
            .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                             vk::AccessFlagBits2::eColorAttachmentRead,
            .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_bloom.handle(),
            .subresourceRange = mipRange(target),
        };

        cmd.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &toAttachment,
        });

        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_bloomUpPipeline->layout(), 0,
                               m_bloomMipSets[level], {});

        const BloomUpPush push{
            .texelSize = Vec2{1.0f / static_cast<f32>(source.width),
                              1.0f / static_cast<f32>(source.height)},
            .radius = 1.0f,
        };

        cmd.pushConstants<BloomUpPush>(m_bloomUpPipeline->layout(),
                                       vk::ShaderStageFlagBits::eFragment, 0, push);

        // eLoad, not eDontCare: the downsampled level is still in there and the
        // blend adds to it. Discarding it would throw away every level above
        // this one and leave only the blurriest.
        blit(target, *m_bloomMipViews[target], bloomMipExtent(target),
             vk::AttachmentLoadOp::eLoad);
    }
}

void Renderer::recordTonemap(vk::CommandBuffer cmd) {
    const vk::Extent2D extent{m_viewportExtent.width, m_viewportExtent.height};

    // The accumulated scene was already made readable by the bloom pass, which
    // runs first and needs it too. All that is left is the image being written.
    const vk::ImageMemoryBarrier2 toAttachment{
        // Waits for the interface's fragment shader, which sampled this image
        // last frame to draw the viewport panel.
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
    };

    cmd.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &toAttachment,
    });

    const vk::RenderingAttachmentInfo colorAttachment{
        .imageView = m_sceneColor.view(),
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        // eDontCare rather than eClear: every pixel is written by the draw
        // below, so clearing first would be wasted bandwidth.
        .loadOp = vk::AttachmentLoadOp::eDontCare,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };

    // No depth attachment at all - which is also why the tone mapping pipeline
    // is built with depthFormat left undefined.
    cmd.beginRendering(vk::RenderingInfo{
        .renderArea = vk::Rect2D{.offset = {0, 0}, .extent = extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachment,
    });

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_tonemapPipeline->handle());
    cmd.setViewport(0, vk::Viewport{
                           .x = 0.0f,
                           .y = 0.0f,
                           .width = static_cast<f32>(extent.width),
                           .height = static_cast<f32>(extent.height),
                           .minDepth = 0.0f,
                           .maxDepth = 1.0f,
                       });
    cmd.setScissor(0, vk::Rect2D{.offset = {0, 0}, .extent = extent});

    // Set 0 for the exposure, set 1 for the image being tone mapped.
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_tonemapPipeline->layout(), 0,
                           m_perFrame[m_frames->currentFrame()].cameraSet, {});
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_tonemapPipeline->layout(), 1,
                           m_tonemapSets[m_accumIndex], {});

    cmd.draw(3, 1, 0, 0);

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

    // A chain of passes, each reading what the one before it wrote - which is
    // exactly why they cannot be merged. The scene goes into the HDR target,
    // the temporal filter averages it with the frames before it, the bloom
    // chain extracts its glow, the tone mapper turns the result into a
    // displayable image, and the interface draws into the window with that
    // image as one of its textures.
    // Before anything is drawn: the rays traced while shading need this
    // frame's picture of the scene to already exist.
    recordAccelerationStructure(cmd, m_frames->currentFrame());

    recordSceneRendering(cmd);

    // Before the bloom, and that order is not a preference: the glow is built
    // from the bright parts of the picture, and a speckle of sampling noise one
    // frame is a bright part the next. Blurring noise before averaging it away
    // would spread it over the screen instead.
    recordTemporal(cmd);

    recordBloom(cmd);
    recordTonemap(cmd);
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

    // Which also means the frames before it are done, and with them anything
    // that could still have been reading a mesh replaced since.
    releaseRetiredMeshes();

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

    // Before anything is recorded, and only when something actually added a
    // texture - so a load that creates twenty of them costs one refresh rather
    // than twenty.
    if (m_textureArrayDirty) {
        refreshTextureArray();
    }

    updateFrameUniforms(m_frames->currentFrame());

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
