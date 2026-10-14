#include "fumar/render/renderer.hpp"

#include "cpu_workload.hpp"

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
#include "fumar/render/texture_set.hpp"
#include "fumar/rhi/upload_context.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdlib>
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
/// 96 bytes of the 128 every implementation guarantees. What a surface looks
/// like is no longer here: it is a record in the material buffer, and a draw
/// names it by index - which is what lets a ray, which has no push constants,
/// see the same material the camera does.
struct ObjectPushConstants {
    Mat4 model;         // 64 bytes, offset 0

    /// The flat colour the outline pass draws light markers with. The mesh
    /// pipeline ignores it.
    Vec4 overlayColor;  // 16,       offset 64

    u32 materialIndex;  // 4,        offset 80
    f32 highlight;      // 4,        offset 84: 0 = normal, 0.5 = hovered, 1 = selected, < 0 = overlay colour
    u32 nodeIndex = 0;  // 4,        offset 88: GPU workload, index into the world-matrix buffer
    u32 pad = 0;        // 4,        offset 92
};

static_assert(sizeof(ObjectPushConstants) == 96);
static_assert(offsetof(ObjectPushConstants, overlayColor) == 64);
static_assert(offsetof(ObjectPushConstants, materialIndex) == 80);
static_assert(offsetof(ObjectPushConstants, highlight) == 84);
static_assert(offsetof(ObjectPushConstants, nodeIndex) == 88);

/// One node for world.comp. 80 bytes, which is the std430 stride of a mat4
/// followed by four uints. parent is kInvalidNode at the root.
struct GpuNode {
    Mat4 local;
    u32 parent = kInvalidNode;
    u32 pad0 = 0;
    u32 pad1 = 0;
    u32 pad2 = 0;
};
static_assert(sizeof(GpuNode) == 80);
static_assert(offsetof(GpuNode, parent) == 64);

/// How many material records each frame's buffer holds before it grows.
constexpr u32 kInitialMaterialCapacity = 256;

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
    m_profiler.init(*m_device, rhi::kFramesInFlight);

    createDefaultTexture();
    createViewportTarget(m_viewportExtent);
    chooseTextureSlots();
    createDescriptors();

    const std::array<vk::VertexInputBindingDescription, 1> vertexBindings{Vertex::binding()};
    const auto vertexAttributes = Vertex::attributes();
    // One set: the frame's, which now carries the materials and every texture
    // too. A draw picks its material by index rather than by binding a set.
    const std::array<vk::DescriptorSetLayout, 1> setLayouts{*m_cameraSetLayout};

    const std::filesystem::path shaderDir = executableDirectory() / "shaders";

    // Same shading, two builds of it: the ray query variant traces for shadows
    // and occlusion, the plain one assumes everything is lit and open. Choosing
    // between compiled shaders rather than branching inside one keeps the
    // fallback free of any cost, and is the only option anyway - an extension
    // has to be declared when the shader is compiled, not when it runs.
    // The texture array's size, for the shaders that declare it.
    const std::array<u32, 1> textureSlotConstant{m_textureSlots};

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
                       .fragmentConstants = textureSlotConstant,
                   });

    const std::array<vk::DescriptorSetLayout, 2> gpuSetLayouts{*m_cameraSetLayout, *m_worldSetLayout};
    m_gpuPipeline = std::make_unique<rhi::GraphicsPipeline>(
        *m_device, rhi::GraphicsPipelineDesc{
                       .vertexShader = shaderDir / "mesh_gpu.vert.spv",
                       .fragmentShader = meshFragment,
                       .colorFormat = kSceneHdrFormat,
                       .depthFormat = m_depthFormat,
                       .vertexBindings = vertexBindings,
                       .vertexAttributes = vertexAttributes,
                       .setLayouts = gpuSetLayouts,
                       .pushConstantSize = sizeof(ObjectPushConstants),
                       .pushConstantStages = vk::ShaderStageFlagBits::eVertex |
                                             vk::ShaderStageFlagBits::eFragment,
                       .samples = m_sampleCount,
                       .fragmentConstants = textureSlotConstant,
                   });

    // The Cpu workload's: binding 1 is a second stream, CpuLightVertex, read
    // at locations 4 to 6 beside the ordinary vertex.
    const std::array<vk::VertexInputBindingDescription, 2> cpuBindings{
        Vertex::binding(),
        vk::VertexInputBindingDescription{
            .binding = 1,
            .stride = sizeof(CpuLightVertex),
            .inputRate = vk::VertexInputRate::eVertex,
        },
    };
    std::vector<vk::VertexInputAttributeDescription> cpuAttributes(vertexAttributes.begin(), vertexAttributes.end());
    for (u32 i = 0; i < 3; ++i) {
        cpuAttributes.push_back(vk::VertexInputAttributeDescription{
            .location = 4 + i,
            .binding = 1,
            .format = vk::Format::eR32G32B32A32Sfloat,
            .offset = i * static_cast<u32>(sizeof(Vec4)),
        });
    }
    m_cpuPipeline = std::make_unique<rhi::GraphicsPipeline>(
        *m_device, rhi::GraphicsPipelineDesc{
                       .vertexShader = shaderDir / "mesh_cpu.vert.spv",
                       .fragmentShader = shaderDir / "mesh_cpu.frag.spv",
                       .colorFormat = kSceneHdrFormat,
                       .depthFormat = m_depthFormat,
                       .vertexBindings = cpuBindings,
                       .vertexAttributes = cpuAttributes,
                       .setLayouts = setLayouts,
                       .pushConstantSize = sizeof(ObjectPushConstants),
                       .pushConstantStages = vk::ShaderStageFlagBits::eVertex |
                                             vk::ShaderStageFlagBits::eFragment,
                       .samples = m_sampleCount,
                       .fragmentConstants = textureSlotConstant,
                   });

    const std::array<vk::DescriptorSetLayout, 1> computeSetLayouts{*m_computeSetLayout};
    m_worldCompute = std::make_unique<rhi::ComputePipeline>(
        *m_device, rhi::ComputePipelineDesc{
                       .shader = shaderDir / "world.comp.spv",
                       .setLayouts = computeSetLayouts,
                       .pushConstantSize = sizeof(u32),
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
    // than the one-image layout the bloom passes use.
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

    // --- the floor grid -----------------------------------------------------
    // Drawn inside the scene pass, after the geometry: tested against its
    // depth so the object hides the grid, but writing none, so nothing drawn
    // later is hidden by a floor that is only lines. One set, because all it
    // needs is the camera.
    const std::array<vk::DescriptorSetLayout, 1> gridSetLayouts{*m_cameraSetLayout};
    m_gridPipeline = std::make_unique<rhi::GraphicsPipeline>(
        *m_device, rhi::GraphicsPipelineDesc{
                       .vertexShader = shaderDir / "fullscreen.vert.spv",
                       .fragmentShader = shaderDir / "grid.frag.spv",
                       .colorFormat = kSceneHdrFormat,
                       .depthFormat = m_depthFormat,
                       .vertexBindings = {},
                       .vertexAttributes = {},
                       .setLayouts = gridSetLayouts,
                       .samples = m_sampleCount,
                       .alphaBlend = true,
                       .cullMode = vk::CullModeFlagBits::eNone,
                       .depthTest = true,
                       .depthWrite = false,
                       // Equal as well: the floor point found for a pixel can
                       // land exactly on the depth of a mesh resting on it.
                       .depthCompare = vk::CompareOp::eLessOrEqual,
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
    const std::array<vk::DescriptorSetLayout, 1> bloomSetLayouts{*m_imageSetLayout};

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
    m_imageSetLayout.reset();
    m_cameraSetLayout.reset();
    m_descriptorPool.reset();
    m_profiler.shutdown();
    m_frames.reset();
    m_bloomUpPipeline.reset();
    m_bloomDownPipeline.reset();
    m_temporalPipeline.reset();
    m_gridPipeline.reset();
    m_tonemapPipeline.reset();
    m_skyPipeline.reset();
    m_outlinePipeline.reset();
    m_cpu.reset();
    m_cpuPipeline.reset();
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
    }

    // Every texture in the scene and every material, on every GPU: the
    // rasteriser picks its material by index the same way a ray does. A fixed
    // array rather than one sized at runtime - a variable-count binding is
    // another feature to require - with the slots past the scene holding the
    // white neutral texture, so nothing ever reads a descriptor nobody wrote.
    frameLayout.binding(3, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment,
                        m_textureSlots);
    frameLayout.binding(4, vk::DescriptorType::eStorageBuffer,
                        vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment);
    m_cameraSetLayout = frameLayout.build(handle);

    m_worldSetLayout = rhi::DescriptorSetLayoutBuilder()
                           .binding(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eVertex)
                           .build(handle);
    m_computeSetLayout =
        rhi::DescriptorSetLayoutBuilder()
            .binding(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
            .binding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
            .build(handle);

    m_imageSetLayout = rhi::DescriptorSetLayoutBuilder()
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

    // The image sets the renderer keeps for itself: one per level of the bloom
    // chain, so a pass can bind the level below it as its source without
    // rewriting a descriptor mid-frame, and then two each for the tone mapper,
    // the bloom chain's first source and the temporal pass - two because the
    // accumulated image alternates between a pair, and a descriptor cannot be
    // rewritten mid-flight.
    constexpr u32 kInternalImageSets = 6 + kBloomMips;

    // More descriptors than sets, because three of those sets hold more than
    // one image each: the tone mapper reads the scene and the glow, and the
    // temporal pass reads the scene, the history and the depth.
    constexpr u32 kInternalImageDescriptors = 2 * (2 + 1 + 3) + kBloomMips;

    // Every frame's set holds the whole texture array and the material buffer.
    std::vector<vk::DescriptorPoolSize> poolSizes{
        vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, rhi::kFramesInFlight},
        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler,
                               kInternalImageDescriptors + m_textureSlots * rhi::kFramesInFlight},
        // Materials, plus the GPU-workload node buffer, world buffer (compute)
        // and the same world buffer bound again for the vertex shader.
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, rhi::kFramesInFlight * 4},
    };
    if (m_device->rayTracingSupported()) {
        poolSizes.push_back(vk::DescriptorPoolSize{vk::DescriptorType::eAccelerationStructureKHR,
                                                   rhi::kFramesInFlight});
        // The instance records.
        poolSizes.push_back(
            vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, rhi::kFramesInFlight});
    }
    m_descriptorPool = std::make_unique<rhi::DescriptorPool>(
        *m_device, rhi::kFramesInFlight * 3 + kInternalImageSets, poolSizes);

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
        // Host visible: records are rewritten in place when a material
        // changes, a few at a time, by the CPU.
        frame.materialCapacity = kInitialMaterialCapacity;
        frame.materialData = rhi::Buffer(*m_device, rhi::BufferDesc{
                                                        .size = sizeof(MaterialRecord) * frame.materialCapacity,
                                                        .usage = vk::BufferUsageFlagBits::eStorageBuffer,
                                                        .hostVisible = true,
                                                    });
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

    // A starting size. recordWorldCompute grows both if the scene outgrows it,
    // after this frame's fence, and rewrites the descriptors then.
    constexpr u32 kInitialGpuNodes = 64;
    for (PerFrame& frame : m_perFrame) {
        frame.gpuNodes = rhi::Buffer(*m_device, rhi::BufferDesc{
                                                    .size = sizeof(GpuNode) * kInitialGpuNodes,
                                                    .usage = vk::BufferUsageFlagBits::eStorageBuffer,
                                                    .hostVisible = true,
                                                });
        frame.gpuWorlds = rhi::Buffer(*m_device, rhi::BufferDesc{
                                                     .size = sizeof(Mat4) * kInitialGpuNodes,
                                                     .usage = vk::BufferUsageFlagBits::eStorageBuffer,
                                                     .hostVisible = false,
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
        writer.buffer(frame.cameraSet, 4, frame.materialData.handle(), frame.materialData.size(),
                      vk::DescriptorType::eStorageBuffer);

        frame.computeSet = m_descriptorPool->allocate(*m_computeSetLayout);
        writer.buffer(frame.computeSet, 0, frame.gpuNodes.handle(), frame.gpuNodes.size(),
                      vk::DescriptorType::eStorageBuffer);
        writer.buffer(frame.computeSet, 1, frame.gpuWorlds.handle(), frame.gpuWorlds.size(),
                      vk::DescriptorType::eStorageBuffer);

        frame.worldSet = m_descriptorPool->allocate(*m_worldSetLayout);
        writer.buffer(frame.worldSet, 0, frame.gpuWorlds.handle(), frame.gpuWorlds.size(),
                      vk::DescriptorType::eStorageBuffer);

        // Every slot, once: the binding is read as a whole, and a slot nobody
        // wrote is undefined behaviour rather than a visible mistake. The
        // scene's own textures replace these as they arrive.
        for (u32 slot = 0; slot < m_textureSlots; ++slot) {
            writer.image(frame.cameraSet, 3, m_defaultTexture.view(), *m_sampler,
                         vk::ImageLayout::eShaderReadOnlyOptimal, slot);
        }
        frame.texturesWritten = 0;
        frame.materialDirty.clear();
    }

    // In pairs, because the image these name is one of the two the temporal
    // filter alternates between. Which one is current changes every frame, and
    // rewriting a descriptor a frame in flight may be reading is not allowed -
    // so both exist and the recording picks one.
    for (u32 slot = 0; slot < 2; ++slot) {
        m_tonemapSets[slot] = m_descriptorPool->allocate(*m_postSetLayout);
        m_bloomSourceSets[slot] = m_descriptorPool->allocate(*m_imageSetLayout);
        m_temporalSets[slot] = m_descriptorPool->allocate(*m_temporalSetLayout);
    }

    // One set per level of the bloom chain, allocated once here and rewritten
    // on a resize. Allocated once because the pool cannot free individual sets,
    // so allocating on every resize would drain it in a few drags of the panel
    // edge.
    m_bloomMipSets.resize(kBloomMips);
    for (vk::DescriptorSet& set : m_bloomMipSets) {
        set = m_descriptorPool->allocate(*m_imageSetLayout);
    }

    // Fills in all of the above. Everything they point at was created by
    // createViewportTarget, which runs before this.
    updatePostDescriptors();

    writer.submit(m_device->handle());

    registerBuiltins();
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

void Renderer::chooseTextureSlots() {
    // Every combined image sampler in the array counts against four limits at
    // once - per stage and per set, as a sampler and as an image - and the
    // tightest wins. A few are left over for anything else a pipeline binds.
    const vk::PhysicalDeviceLimits& limits = m_device->properties().limits;
    u32 slots = std::min({kMaxSceneTextures, limits.maxPerStageDescriptorSamplers,
                          limits.maxPerStageDescriptorSampledImages, limits.maxDescriptorSetSamplers,
                          limits.maxDescriptorSetSampledImages});
    slots = slots > 16 ? slots - 8 : slots;

    // _dupenv_s rather than getenv, which MSVC's runtime deprecates.
#if defined(_WIN32)
    char* forced = nullptr;
    usize length = 0;
    if (_dupenv_s(&forced, &length, "FUMAR_TEXTURE_SLOTS") != 0) {
        forced = nullptr;
    }
#else
    const char* forced = std::getenv("FUMAR_TEXTURE_SLOTS");
#endif
    if (forced != nullptr) {
        const long value = std::strtol(forced, nullptr, 10);
        if (value >= 4 && static_cast<u32>(value) < slots) {
            slots = static_cast<u32>(value);
        }
#if defined(_WIN32)
        std::free(forced);
#endif
    }
    m_textureSlots = slots;
    if (m_textureSlots < kMaxSceneTextures) {
        FUMAR_INFO("texture array: {} slots (the GPU's descriptor limits, or FUMAR_TEXTURE_SLOTS)", m_textureSlots);
    }
}

void Renderer::registerBuiltins() {
    // Into an empty registry, so they take the first slots every time. A slot
    // with no map of its own reads one of these: white multiplies nothing away
    // - the factor alone comes through - and (0.5, 0.5, 1) is a normal map
    // that tilts nothing.
    const auto solid = [this](const char* name, std::array<u8, 4> rgba, bool srgb) {
        rhi::Image image(*m_device, rhi::ImageDesc{
                                        .extent = vk::Extent2D{1, 1},
                                        .format = srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm,
                                        .usage = vk::ImageUsageFlagBits::eSampled |
                                                 vk::ImageUsageFlagBits::eTransferDst,
                                        .aspect = vk::ImageAspectFlagBits::eColor,
                                    });
        m_upload->uploadImage(image, rgba.data(), rgba.size());
        const TextureHandle handle = m_resources.addTexture(std::move(image));
        FUMAR_DEBUG("neutral texture '{}' in slot {}", name, handle.index);
        return handle.index;
    };

    {
        const rhi::UploadBatch batch(*m_upload);
        m_neutral.white = solid("white", {255, 255, 255, 255}, true);
        m_neutral.flatNormal = solid("flat normal", {128, 128, 255, 255}, false);
    }
    m_warnedTextureOverflow = false;

    // The fallback material: used by anything with no material of its own, so
    // an untextured or broken asset still draws as something.
    Material fallback;
    fallback.name = "default";
    fallback.baseColorFactor = Vec4{0.30f, 0.31f, 0.33f, 1.0f};
    m_resources.setFallbackMaterial(m_resources.addMaterial(std::move(fallback)));
}

void Renderer::markMaterialDirty(MaterialHandle handle) {
    if (!m_resources.has(handle)) {
        return;
    }
    for (PerFrame& frame : m_perFrame) {
        // Past the end of the list means new, and new is dirty already.
        if (handle.index < frame.materialDirty.size()) {
            frame.materialDirty[handle.index] = true;
        }
    }
}

void Renderer::updateMaterialData(u32 frameIndex) {
    PerFrame& frame = m_perFrame[frameIndex];
    rhi::DescriptorWriter writer;

    // --- textures added since this frame's array was last written ------------
    const usize textureCount = m_resources.textureCount();
    if (textureCount > m_textureSlots && !m_warnedTextureOverflow) {
        FUMAR_WARN("{} textures in the scene; the shaders reach {} - the rest read white", textureCount,
                   m_textureSlots);
        m_warnedTextureOverflow = true;
    }
    const u32 reachable = static_cast<u32>(std::min<usize>(textureCount, m_textureSlots));
    for (u32 slot = frame.texturesWritten; slot < reachable; ++slot) {
        writer.image(frame.cameraSet, 3, m_resources.texture(TextureHandle{slot}).view(), *m_sampler,
                     vk::ImageLayout::eShaderReadOnlyOptimal, slot);
    }
    frame.texturesWritten = std::max(frame.texturesWritten, reachable);

    // --- the record buffer, grown if the materials outgrew it ------------------
    // This frame's buffer, after this frame's fence: nothing is reading it. The
    // other frame keeps its own until its turn comes round.
    const usize materialCount = m_resources.materialCount();
    if (materialCount > frame.materialCapacity) {
        u32 capacity = frame.materialCapacity;
        while (capacity < materialCount) {
            capacity *= 2;
        }
        frame.materialCapacity = capacity;
        frame.materialData = rhi::Buffer(*m_device, rhi::BufferDesc{
                                                        .size = sizeof(MaterialRecord) * capacity,
                                                        .usage = vk::BufferUsageFlagBits::eStorageBuffer,
                                                        .hostVisible = true,
                                                    });
        writer.buffer(frame.cameraSet, 4, frame.materialData.handle(), frame.materialData.size(),
                      vk::DescriptorType::eStorageBuffer);
        // A new buffer holds nothing yet.
        frame.materialDirty.assign(materialCount, true);
    }

    // --- dirty and new records -------------------------------------------------------
    frame.materialDirty.resize(materialCount, true);
    for (u32 i = 0; i < materialCount; ++i) {
        if (!frame.materialDirty[i]) {
            continue;
        }
        const MaterialRecord record =
            toRecord(m_resources.material(MaterialHandle{i}), m_neutral, textureCount, m_textureSlots);
        frame.materialData.write(&record, sizeof(record), sizeof(MaterialRecord) * i);
        frame.materialDirty[i] = false;
    }

    writer.submit(m_device->handle());
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
    // A folder is a texture set - see importMaterialFolder.
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return true;
    }
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

    if (std::filesystem::is_directory(path, ec)) {
        importMaterialFolder(path);
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

    // Every handle in it pointed into the registry just emptied: a .fmat asked
    // for again must be loaded again, not handed an index that is now
    // somebody else's.
    m_materialFiles.clear();
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

    material.origin = MaterialOrigin{.kind = MaterialOrigin::Kind::Builtin, .file = pathToUtf8(baseColorTexture), .index = 0};

    if (!baseColorTexture.empty()) {
        rhi::Image texture = loadTextureFromFile(*m_device, *m_upload, baseColorTexture);
        if (texture.valid()) {
            material.baseColor = m_resources.addTexture(std::move(texture));
            material.baseColorPath = pathToUtf8(baseColorTexture);
        } else {
            FUMAR_WARN("material '{}' falls back to a flat colour", material.name);
        }
    }

    return m_resources.addMaterial(std::move(material));
}

TextureHandle Renderer::loadMaterialTexture(const std::string& material, const char* slot,
                                           const std::filesystem::path& file, bool srgb) {
    if (file.empty()) {
        return {};
    }
    // Asked before anything is decoded: a file larger than the GPU can hold
    // as one texture would be read into memory whole and then refused anyway.
    const std::optional<ImageSize> size = readImageSize(file);
    if (!size) {
        FUMAR_WARN("material '{}': cannot read {} '{}', using the neutral map", material, slot, pathToUtf8(file));
        return {};
    }
    const u32 limit = m_device->properties().limits.maxImageDimension2D;
    if (size->width > limit || size->height > limit) {
        FUMAR_WARN("material '{}': {} '{}' is {}x{}, larger than the GPU's {}; using the neutral map", material,
                   slot, pathToUtf8(file), size->width, size->height, limit);
        return {};
    }
    std::optional<Pixels> pixels = loadPixels(file);
    if (!pixels) {
        FUMAR_WARN("material '{}': cannot decode {} '{}', using the neutral map", material, slot, pathToUtf8(file));
        return {};
    }
    rhi::Image image =
        createTextureFromPixels(*m_device, *m_upload, pixels->rgba.data(), pixels->width, pixels->height, srgb);
    return image.valid() ? m_resources.addTexture(std::move(image)) : TextureHandle{};
}

MaterialHandle Renderer::createMaterial(const MaterialDesc& desc, MaterialOrigin origin) {
    return m_resources.addMaterial(buildMaterial(desc, std::move(origin)));
}

void Renderer::rebuildMaterial(MaterialHandle handle, const MaterialDesc& desc) {
    if (!m_resources.has(handle)) {
        return;
    }
    const Material previous = m_resources.material(handle);
    m_resources.material(handle) = buildMaterial(desc, previous.origin, &previous);
    markMaterialDirty(handle);
}

Material Renderer::buildMaterial(const MaterialDesc& desc, MaterialOrigin origin, const Material* previous) {
    Material material;
    material.name = desc.name;
    material.origin = std::move(origin);
    material.baseColorFactor = desc.baseColorFactor;
    material.metallic = desc.metallicFactor;
    material.roughness = desc.roughnessFactor;
    material.normalScale = desc.normalScale;
    material.occlusionStrength = desc.occlusionStrength;
    material.emissiveColor = desc.emissiveColor;
    material.emissiveIntensity = desc.emissiveIntensity;
    material.alphaMode = desc.alphaMode;
    material.alphaCutoff = desc.alphaCutoff;
    material.uvScale = desc.uvScale;

    // Kept whether or not the files load: a material saved again should still
    // point at a map that is missing today, so putting the file back fixes it.
    material.baseColorPath = pathToUtf8(desc.baseColor);
    material.packedMetallicRoughnessPath = pathToUtf8(desc.packedMetallicRoughness);
    material.metallicPath = pathToUtf8(desc.metallic);
    material.roughnessPath = pathToUtf8(desc.roughness);
    material.normalPath = pathToUtf8(desc.normal);
    material.normalIsDirectX = desc.normalIsDirectX;
    material.occlusionPath = pathToUtf8(desc.occlusion);
    material.emissivePath = pathToUtf8(desc.emissive);

    // One submit for every map of the material, rather than one each.
    const rhi::UploadBatch batch(*m_upload);

    // The previous material's texture for a slot whose source has not changed
    // and which did load: no second copy of the same image on the GPU.
    const auto reuse = [&](TextureHandle before, bool unchanged) {
        return previous != nullptr && unchanged && m_resources.has(before) ? before : TextureHandle{};
    };

    material.baseColor = reuse(previous ? previous->baseColor : TextureHandle{},
                               previous && previous->baseColorPath == material.baseColorPath);
    if (!material.baseColor.valid()) {
        material.baseColor = loadMaterialTexture(material.name, "colour map", desc.baseColor, true);
    }
    material.emissive = reuse(previous ? previous->emissive : TextureHandle{},
                              previous && previous->emissivePath == material.emissivePath);
    if (!material.emissive.valid()) {
        material.emissive = loadMaterialTexture(material.name, "emission map", desc.emissive, true);
    }
    material.occlusion = reuse(previous ? previous->occlusion : TextureHandle{},
                               previous && previous->occlusionPath == material.occlusionPath);
    if (!material.occlusion.valid()) {
        material.occlusion = loadMaterialTexture(material.name, "occlusion map", desc.occlusion, false);
    }
    material.metallicRoughness =
        reuse(previous ? previous->metallicRoughness : TextureHandle{},
              previous && previous->packedMetallicRoughnessPath == material.packedMetallicRoughnessPath &&
                  previous->roughnessPath == material.roughnessPath && previous->metallicPath == material.metallicPath);
    material.normal = reuse(previous ? previous->normal : TextureHandle{},
                            previous && previous->normalPath == material.normalPath &&
                                previous->normalIsDirectX == material.normalIsDirectX);

    if (material.metallicRoughness.valid()) {
        // Kept from before.
    } else if (!desc.packedMetallicRoughness.empty()) {
        material.metallicRoughness =
            loadMaterialTexture(material.name, "metal-rough map", desc.packedMetallicRoughness, false);
    } else if (!desc.roughness.empty() || !desc.metallic.empty()) {
        // Two images into one, in glTF's layout, so the shader reads one map.
        const auto load = [&](const std::filesystem::path& file, const char* slot) -> std::optional<Pixels> {
            if (file.empty()) {
                return std::nullopt;
            }
            const std::optional<ImageSize> size = readImageSize(file);
            const u32 limit = m_device->properties().limits.maxImageDimension2D;
            if (!size || size->width > limit || size->height > limit) {
                FUMAR_WARN("material '{}': cannot use {} '{}', using the neutral map", material.name, slot,
                           pathToUtf8(file));
                return std::nullopt;
            }
            std::optional<Pixels> pixels = loadPixels(file);
            if (!pixels) {
                FUMAR_WARN("material '{}': cannot decode {} '{}', using the neutral map", material.name, slot,
                           pathToUtf8(file));
            }
            return pixels;
        };
        const std::optional<Pixels> roughness = load(desc.roughness, "roughness map");
        const std::optional<Pixels> metallic = load(desc.metallic, "metalness map");
        if (roughness || metallic) {
            const Pixels packed =
                packMetallicRoughness(roughness ? &*roughness : nullptr, metallic ? &*metallic : nullptr);
            rhi::Image image = createTextureFromPixels(*m_device, *m_upload, packed.rgba.data(), packed.width,
                                                       packed.height, false);
            if (image.valid()) {
                material.metallicRoughness = m_resources.addTexture(std::move(image));
            }
        }
    }

    if (material.normal.valid()) {
        // Kept from before.
    } else if (desc.normalIsDirectX && !desc.normal.empty()) {
        // DirectX normal maps point green down; fumar's frame points it up.
        // Flipped once, here, rather than in every shader that reads it. The
        // size is asked before anything is decoded, as for every other map.
        const std::optional<ImageSize> size = readImageSize(desc.normal);
        const u32 limit = m_device->properties().limits.maxImageDimension2D;
        std::optional<Pixels> pixels;
        if (size && size->width <= limit && size->height <= limit) {
            pixels = loadPixels(desc.normal);
        }
        if (pixels) {
            for (usize i = 1; i < pixels->rgba.size(); i += 4) {
                pixels->rgba[i] = static_cast<u8>(255 - pixels->rgba[i]);
            }
            rhi::Image image = createTextureFromPixels(*m_device, *m_upload, pixels->rgba.data(), pixels->width,
                                                       pixels->height, false);
            if (image.valid()) {
                material.normal = m_resources.addTexture(std::move(image));
            }
        } else {
            FUMAR_WARN("material '{}': cannot use normal map '{}', using the neutral map", material.name,
                       pathToUtf8(desc.normal));
        }
    } else {
        material.normal = loadMaterialTexture(material.name, "normal map", desc.normal, false);
    }

    return material;
}

MaterialHandle Renderer::loadMaterialFile(const std::filesystem::path& file) {
    std::error_code ec;
    const std::string key = pathToUtf8(std::filesystem::weakly_canonical(std::filesystem::absolute(file), ec));
    if (const auto found = m_materialFiles.find(key); found != m_materialFiles.end()) {
        return found->second;
    }

    const std::optional<MaterialDesc> desc = readMaterialFile(file);
    if (!desc) {
        return {};
    }
    const MaterialHandle handle =
        createMaterial(*desc, MaterialOrigin{.kind = MaterialOrigin::Kind::Fmat, .file = key, .index = 0});
    m_materialFiles.emplace(key, handle);
    FUMAR_INFO("material '{}' loaded from '{}'", desc->name, pathToUtf8(pathFromUtf8(key).filename()));
    return handle;
}

bool Renderer::saveMaterialFile(MaterialHandle handle) {
    if (!m_resources.has(handle)) {
        return false;
    }
    const Material& material = m_resources.material(handle);
    if (material.origin.kind != MaterialOrigin::Kind::Fmat || material.origin.file.empty()) {
        return false;
    }
    return writeMaterialFile(material.origin.file, describeMaterial(material));
}

MaterialHandle Renderer::importMaterialFolder(const std::filesystem::path& folder) {
    const std::filesystem::path normalized = std::filesystem::absolute(folder).lexically_normal();
    // A trailing separator leaves the last component empty; the name is the
    // folder's own.
    const std::filesystem::path base = normalized.has_filename() ? normalized : normalized.parent_path();
    std::filesystem::path fmatName = base.filename();
    fmatName += ".fmat";
    const std::filesystem::path fmat = base / fmatName;

    std::error_code ec;
    if (std::filesystem::exists(fmat, ec)) {
        return loadMaterialFile(fmat);
    }

    const TextureSetFiles set = findTextureSet(base);
    if (!set.usable()) {
        FUMAR_WARN("'{}' holds no colour or normal map; no material made from it", pathToUtf8(base));
        return {};
    }

    MaterialDesc desc;
    desc.name = pathToUtf8(base.filename());
    desc.baseColor = set.baseColor;
    desc.normal = set.normal;
    desc.normalIsDirectX = set.normalIsDirectX;
    desc.roughness = set.roughness;
    desc.metallic = set.metallic;
    desc.occlusion = set.occlusion;
    desc.emissive = set.emissive;
    // The maps carry the values; the factors only pass them through.
    desc.roughnessFactor = 1.0f;
    desc.metallicFactor = set.metallic.empty() ? 0.0f : 1.0f;
    if (!set.emissive.empty()) {
        desc.emissiveColor = Vec3{1.0f, 1.0f, 1.0f};
    }

    if (!writeMaterialFile(fmat, desc)) {
        return {};
    }
    FUMAR_INFO("material '{}' made from '{}'", desc.name, pathToUtf8(base));
    return loadMaterialFile(fmat);
}

NodeId Renderer::loadModel(const std::filesystem::path& path, NodeId parent) {
    const GltfLoadContext context{
        .device = *m_device,
        .upload = *m_upload,
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

        // A ray that starts inside a box does not hit that box. The box it
        // starts in is nearly always the caller's own - a character probing
        // for the floor from its middle - and counting it would answer
        // "distance 0" to every such question. Physics engines draw the line
        // in the same place.
        const Bounds& bounds = m_resources.mesh(node.mesh).bounds();
        const bool inside = localOrigin.x >= bounds.min.x && localOrigin.x <= bounds.max.x &&
                            localOrigin.y >= bounds.min.y && localOrigin.y <= bounds.max.y &&
                            localOrigin.z >= bounds.min.z && localOrigin.z <= bounds.max.z;
        if (inside) {
            return;
        }

        f32 distance = 0.0f;
        if (!intersectRayBounds(localOrigin, localDirection, bounds, distance)) {
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

    // Whether anything traces rays this frame: the GPU when it can, or the
    // CPU in its own workload - which works on any GPU at all.
    const bool trace = m_device->rayTracingSupported() || m_workload == Workload::Cpu;

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
        .shadowStrength = trace ? env.shadowStrength : 0.0f,
        .occlusionStrength = trace ? env.occlusionStrength : 0.0f,
        .occlusionRadius = env.occlusionRadius,
        .reflectionStrength = trace ? env.reflectionStrength : 0.0f,
        .reflectionRoughnessLimit = env.reflectionRoughnessLimit,
        .indirectStrength = trace ? env.indirectStrength : 0.0f,
        .bloomStrength = env.bloomStrength,
        .bloomThreshold = env.bloomThreshold,
        // Zero on the frame after the images were created, because there is no
        // history to carry over yet - the pair holds uninitialised memory, and
        // averaging that in would be a flash of whatever was in it. Zero means
        // "this frame only", which is exactly right once.
        //
        // Also zero when the GPU cannot trace: with no rays there is no
        // sampling noise, and nothing left for the filter to do but add lag.
        .temporalStrength = (m_historyValid && trace) ? env.temporalStrength : 0.0f,
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
                          (light.castsShadows && trace) ? 1.0f : 0.0f},
        };
        ++lightCount;
    });

    uniforms.lightCount = static_cast<i32>(lightCount);

    // The same picture of the lights and the sky for the CPU's rays.
    if (m_workload == Workload::Cpu) {
        if (!m_cpu) {
            m_cpu = std::make_unique<CpuWorkload>();
        }
        CpuLightInputs& in = m_cpu->inputs;
        in.sunDirection = env.sunDirection();
        in.sunAngularRadius = uniforms.sunAngularRadius;
        in.sunRadiance = env.sunColor * env.sunIntensity;
        in.skyZenith = env.skyZenithColor;
        in.skyHorizon = env.skyHorizonColor;
        in.ground = env.groundColor;
        in.skyIntensity = env.skyIntensity;
        in.occlusionRadius = env.occlusionRadius;
        in.lampCount = std::min(lightCount, CpuLightInputs::kMaxLamps);
        for (u32 i = 0; i < in.lampCount; ++i) {
            const LightUniform& light = uniforms.lights[i];
            in.lamps[i] = CpuLightInputs::Lamp{
                .position = Vec3{light.positionRange.x, light.positionRange.y, light.positionRange.z},
                .range = light.positionRange.w,
                .sourceRadius = light.shape.y,
                .castsShadow = light.shape.w > 0.5f,
            };
        }
    }

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

        m_instanceRecords.push_back(InstanceRecord{
            .vertices = mesh.vertexBuffer().deviceAddress(),
            .indices = mesh.indexBuffer().deviceAddress(),
            .materialIndex = handle.valid() ? handle.index : 0,
            .padding = 0,
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

void Renderer::ensureHostBuffer(rhi::Buffer& buffer, vk::DeviceSize bytes, vk::BufferUsageFlags usage) {
    if (buffer.valid() && buffer.size() >= bytes) {
        return;
    }

    // Grow rather than matching the request, so a mesh that gets a few
    // vertices bigger does not allocate again next frame.
    vk::DeviceSize size = bytes;
    if (buffer.valid()) {
        size = std::max(bytes, buffer.size() + buffer.size() / 2);
    }

    buffer = rhi::Buffer(*m_device, rhi::BufferDesc{
                                        .size = size,
                                        .usage = usage,
                                        .hostVisible = true,
                                    });
}

u32 Renderer::cpuLightSamples() const { return m_cpu ? m_cpu->samples() : 0; }

u32 Renderer::cpuLightMaxSamples() { return CpuWorkload::kMaxSamples; }

u32 Renderer::cpuThreads() const { return m_cpu ? m_cpu->threadCount() : 0; }

void Renderer::prepareCpuGeometry(u32 frameIndex) {
    if (!m_cpu) {
        m_cpu = std::make_unique<CpuWorkload>();
    }
    m_cpu->settings.vertexSpacing = m_cpuSettings.vertexSpacing;
    m_cpu->settings.budgetMs = m_cpuSettings.budgetMs;

    std::vector<CpuDrawable> drawables;
    forEachRenderedDrawable([&](NodeId id, const Node& node, const Mat4& world) {
        if (!m_resources.has(node.mesh)) {
            return;
        }
        const Mesh& mesh = m_resources.mesh(node.mesh);
        if (!mesh.valid()) {
            return;
        }
        const MaterialHandle handle =
            m_resources.has(node.material) ? node.material : m_resources.fallbackMaterial();
        if (!m_resources.has(handle)) {
            return;
        }

        // What a ray landing on this object sees. The CPU has no copy of the
        // texture pixels, so a textured colour is taken at half its factor -
        // a stand-in for a typical texture's middle tone. Bounced light is
        // broad and dim enough that the difference rarely shows.
        const Material& material = m_resources.material(handle);
        const f32 textured = material.baseColor.valid() ? 0.5f : 1.0f;
        const Vec3 albedo = Vec3{material.baseColorFactor.x, material.baseColorFactor.y, material.baseColorFactor.z} *
                            textured;
        const Vec3 emissive = material.emissiveColor * material.emissiveIntensity *
                              (material.emissive.valid() ? 0.5f : 1.0f);

        drawables.push_back(CpuDrawable{
            .id = id,
            .mesh = &mesh,
            .world = world,
            .materialIndex = handle.index,
            .highlight = id == m_selected ? 1.0f : 0.0f,
            .albedo = albedo,
            .emissive = emissive,
        });
    });

    // updateFrameUniforms has just run and left this frame's view-projection
    // in m_prevViewProjection, ready for the next frame's motion vectors.
    m_cpu->prepare(drawables, m_prevViewProjection, m_profiler);
    m_cpu->light(m_profiler);

    const FrameProfiler::Scope scope = m_profiler.cpu(ProfileStage::Transform);
    PerFrame& frame = m_perFrame[frameIndex];
    const auto vertices = m_cpu->vertices();
    const auto light = m_cpu->lightVertices();
    const auto indices = m_cpu->indices();
    if (vertices.empty()) {
        return;
    }
    ensureHostBuffer(frame.workloadVertices, vertices.size_bytes(), vk::BufferUsageFlagBits::eVertexBuffer);
    ensureHostBuffer(frame.workloadLight, light.size_bytes(), vk::BufferUsageFlagBits::eVertexBuffer);
    ensureHostBuffer(frame.workloadIndices, indices.size_bytes(), vk::BufferUsageFlagBits::eIndexBuffer);
    frame.workloadVertices.write(vertices.data(), vertices.size_bytes());
    frame.workloadLight.write(light.data(), light.size_bytes());
    frame.workloadIndices.write(indices.data(), indices.size_bytes());
}

void Renderer::ensureGpuNodeBuffers(u32 frameIndex, u32 nodeCount) {
    PerFrame& frame = m_perFrame[frameIndex];
    const vk::DeviceSize nodesBytes = static_cast<vk::DeviceSize>(sizeof(GpuNode)) * nodeCount;
    const vk::DeviceSize worldsBytes = static_cast<vk::DeviceSize>(sizeof(Mat4)) * nodeCount;
    if (frame.gpuNodes.valid() && frame.gpuNodes.size() >= nodesBytes && frame.gpuWorlds.valid() &&
        frame.gpuWorlds.size() >= worldsBytes) {
        return;
    }

    u32 capacity = nodeCount;
    if (frame.gpuNodes.valid()) {
        const u32 have = static_cast<u32>(frame.gpuNodes.size() / sizeof(GpuNode));
        capacity = std::max(nodeCount, have + have / 2);
    }

    frame.gpuNodes = rhi::Buffer(*m_device, rhi::BufferDesc{
                                                .size = sizeof(GpuNode) * capacity,
                                                .usage = vk::BufferUsageFlagBits::eStorageBuffer,
                                                .hostVisible = true,
                                            });
    frame.gpuWorlds = rhi::Buffer(*m_device, rhi::BufferDesc{
                                                 .size = sizeof(Mat4) * capacity,
                                                 .usage = vk::BufferUsageFlagBits::eStorageBuffer,
                                                 .hostVisible = false,
                                             });

    rhi::DescriptorWriter writer;
    writer.buffer(frame.computeSet, 0, frame.gpuNodes.handle(), frame.gpuNodes.size(),
                  vk::DescriptorType::eStorageBuffer);
    writer.buffer(frame.computeSet, 1, frame.gpuWorlds.handle(), frame.gpuWorlds.size(),
                  vk::DescriptorType::eStorageBuffer);
    writer.buffer(frame.worldSet, 0, frame.gpuWorlds.handle(), frame.gpuWorlds.size(),
                  vk::DescriptorType::eStorageBuffer);
    writer.submit(m_device->handle());
}

void Renderer::recordWorldCompute(vk::CommandBuffer cmd, u32 frameIndex) {
    u32 maxId = kRootNode;
    std::vector<NodeId> pending{kRootNode};
    while (!pending.empty()) {
        const NodeId id = pending.back();
        pending.pop_back();
        maxId = std::max(maxId, id);
        for (NodeId child : m_scene.node(id).children) {
            if (m_scene.isAlive(child)) {
                pending.push_back(child);
            }
        }
    }

    const u32 count = maxId + 1;
    ensureGpuNodeBuffers(frameIndex, count);

    std::vector<GpuNode> nodes(count);
    for (GpuNode& node : nodes) {
        node.local = identity();
        node.parent = kInvalidNode;
    }

    pending.push_back(kRootNode);
    while (!pending.empty()) {
        const NodeId id = pending.back();
        pending.pop_back();
        const Node& node = m_scene.node(id);
        nodes[id].local = node.transform.matrix();
        nodes[id].parent = id == kRootNode ? kInvalidNode : node.parent;
        for (NodeId child : node.children) {
            if (m_scene.isAlive(child)) {
                pending.push_back(child);
            }
        }
    }

    PerFrame& frame = m_perFrame[frameIndex];
    frame.gpuNodes.write(nodes.data(), static_cast<usize>(count) * sizeof(GpuNode));

    const vk::BufferMemoryBarrier2 hostToCompute{
        .srcStageMask = vk::PipelineStageFlagBits2::eHost,
        .srcAccessMask = vk::AccessFlagBits2::eHostWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = frame.gpuNodes.handle(),
        .size = VK_WHOLE_SIZE,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &hostToCompute,
    });

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_worldCompute->handle());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_worldCompute->layout(), 0,
                           frame.computeSet, {});
    cmd.pushConstants<u32>(m_worldCompute->layout(), vk::ShaderStageFlagBits::eCompute, 0, count);
    cmd.dispatch((count + 63u) / 64u, 1, 1);

    const vk::BufferMemoryBarrier2 computeToVertex{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = frame.gpuWorlds.handle(),
        .size = VK_WHOLE_SIZE,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &computeToVertex,
    });
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

    // Three vertices, no buffer: fullscreen.vert builds them from gl_VertexIndex.
    cmd.draw(3, 1, 0, 0);

    // --- geometry -----------------------------------------------------------
    // Balanced: CPU world matrix, vertex shader transforms, fragment shader
    // shades and traces. Gpu: the compute pass already wrote the world
    // matrices. Cpu: vertices are already in world space, so the model is
    // identity and the same fragment shader still traces.
    rhi::GraphicsPipeline& geometry = m_workload == Workload::Gpu   ? *m_gpuPipeline
                                      : m_workload == Workload::Cpu ? *m_cpuPipeline
                                                                    : *m_pipeline;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, geometry.handle());

    cmd.setViewport(0, viewport);
    cmd.setScissor(0, scissor);

    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, geometry.layout(), 0, frameSet, {});
    if (m_workload == Workload::Gpu) {
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, geometry.layout(), 1,
                               m_perFrame[m_frames->currentFrame()].worldSet, {});
    }

    if (m_workload == Workload::Cpu) {
        const PerFrame& frame = m_perFrame[m_frames->currentFrame()];
        if (m_cpu && !m_cpu->draws().empty() && frame.workloadVertices.valid()) {
            // Two streams side by side: the vertex, and what the CPU traced
            // for it. One index buffer for every object, offset per draw.
            const std::array<vk::Buffer, 2> streams{frame.workloadVertices.handle(), frame.workloadLight.handle()};
            const std::array<vk::DeviceSize, 2> offsets{0, 0};
            cmd.bindVertexBuffers(0, streams, offsets);
            cmd.bindIndexBuffer(frame.workloadIndices.handle(), 0, vk::IndexType::eUint32);
            for (const CpuWorkload::Draw& draw : m_cpu->draws()) {
                const ObjectPushConstants push{
                    .model = identity(),
                    .overlayColor = Vec4{0.0f, 0.0f, 0.0f, 0.0f},
                    .materialIndex = draw.materialIndex,
                    .highlight = draw.highlight,
                };
                cmd.pushConstants<ObjectPushConstants>(
                    geometry.layout(),
                    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, push);
                cmd.drawIndexed(draw.indexCount, 1, draw.firstIndex, draw.vertexOffset, 0);
            }
        }
    } else {
        forEachRenderedDrawable([&](NodeId id, const Node& node, const Mat4& worldTransform) {
            if (!m_resources.has(node.mesh)) {
                return;
            }

            const MaterialHandle handle =
                m_resources.has(node.material) ? node.material : m_resources.fallbackMaterial();
            if (!m_resources.has(handle)) {
                return;
            }

            // Only the selection tints its faces. Hovering is answered by the
            // wireframe pass below and nothing else, so passing the cursor over a
            // crowded scene does not make objects flash.
            const ObjectPushConstants push{
                .model = m_workload == Workload::Gpu ? identity() : worldTransform,
                .overlayColor = Vec4{0.0f, 0.0f, 0.0f, 0.0f},
                .materialIndex = handle.index,
                .highlight = id == m_selected ? 1.0f : 0.0f,
                .nodeIndex = id,
            };
            cmd.pushConstants<ObjectPushConstants>(
                geometry.layout(),
                vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, push);

            m_resources.mesh(node.mesh).draw(cmd);
        });
    }

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
            .overlayColor = Vec4{1.0f, 1.0f, 1.0f, 1.0f},
            .materialIndex = 0,
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
                .overlayColor = Vec4{tint.x * boost, tint.y * boost, tint.z * boost, 1.0f},
                .materialIndex = 0,
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

        // The outline shader reads nothing from the set, but the layout declares
        // it, so something compatible has to be bound or the draw is invalid.
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_outlinePipeline->layout(), 0,
                               frameSet, {});

        // Hover first, so that when the same object is both, the brighter
        // selected outline is what ends up on top.
        if (m_highlighted != m_selected) {
            drawOutline(m_highlighted, 0.5f);
        }
        drawOutline(m_selected, 1.0f);
    }

    if (m_gridVisible) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_gridPipeline->handle());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_gridPipeline->layout(), 0, frameSet, {});
        cmd.draw(3, 1, 0, 0);
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
    // frame's picture of the scene to already exist. The GPU workload's
    // world matrices have to land first, because the vertex shader reads them.
    //
    // Each pass is bracketed twice: a clock on the CPU around recording it,
    // and a pair of timestamps the GPU writes around running it.
    const u32 frame = m_frames->currentFrame();
    m_profiler.resetQueries(cmd, frame);
    const auto timed = [&](ProfileStage stage, auto&& record) {
        const FrameProfiler::Scope scope = m_profiler.cpu(stage);
        m_profiler.gpuBegin(cmd, frame, stage);
        record();
        m_profiler.gpuEnd(cmd, frame, stage);
    };

    if (m_workload == Workload::Gpu) {
        timed(ProfileStage::Nodes, [&] { recordWorldCompute(cmd, frame); });
    }
    // The Cpu workload traces against its own copy of the scene, so the
    // GPU's structure is not built at all.
    if (m_device->rayTracingSupported() && m_workload != Workload::Cpu) {
        timed(ProfileStage::Structure, [&] { recordAccelerationStructure(cmd, frame); });
    }

    timed(ProfileStage::Scene, [&] { recordSceneRendering(cmd); });

    // Before the bloom, and that order is not a preference: the glow is built
    // from the bright parts of the picture, and a speckle of sampling noise one
    // frame is a bright part the next. Blurring noise before averaging it away
    // would spread it over the screen instead.
    timed(ProfileStage::Temporal, [&] { recordTemporal(cmd); });

    timed(ProfileStage::Bloom, [&] { recordBloom(cmd); });
    timed(ProfileStage::Tonemap, [&] { recordTonemap(cmd); });
    timed(ProfileStage::Interface, [&] { recordUiRendering(cmd, imageIndex); });
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
    {
        const FrameProfiler::Scope scope = m_profiler.cpu(ProfileStage::Wait);
        m_frames->waitForFrameSlot();
    }

    // That frame is finished, so the times its GPU passes wrote can be read.
    m_profiler.collect(m_device->handle(), m_frames->currentFrame());

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
    {
        const FrameProfiler::Scope scope = m_profiler.cpu(ProfileStage::Nodes);
        m_scene.updateWorldTransforms();
    }

    // After this slot's fence, before anything is recorded: the frame about to
    // be drawn sees every material edit and every texture added since its
    // last turn, and nothing the GPU is still reading is touched.
    {
        const FrameProfiler::Scope scope = m_profiler.cpu(ProfileStage::Materials);
        updateMaterialData(m_frames->currentFrame());
        updateFrameUniforms(m_frames->currentFrame());
    }

    // The Cpu workload's vertices, transformed before recording starts so
    // their cost is their own line in the profiler rather than part of the
    // scene pass that draws them.
    if (m_workload == Workload::Cpu) {
        prepareCpuGeometry(m_frames->currentFrame());
    }

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
    {
        const FrameProfiler::Scope scope = m_profiler.cpu(ProfileStage::Present);
        m_device->graphicsQueue().submit2(submitInfo, m_frames->inFlightFence());

        if (!m_swapchain->present(*imageIndex, m_frames->renderFinished(*imageIndex))) {
            m_swapchainDirty = true;
        }
    }

    m_frames->advance();
    m_profiler.endCpuFrame();
}

} // namespace fumar
