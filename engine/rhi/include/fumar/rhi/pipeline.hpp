#pragma once

#include "fumar/core/types.hpp"
#include "fumar/rhi/vk_common.hpp"

#include <filesystem>
#include <span>

namespace fumar::rhi {

class Device;

struct GraphicsPipelineDesc {
    std::filesystem::path vertexShader;
    std::filesystem::path fragmentShader;

    /// Format of the colour attachment. With dynamic rendering there is no
    /// VkRenderPass describing the attachments, so the pipeline is told
    /// directly - and it must match the swapchain exactly.
    vk::Format colorFormat = vk::Format::eUndefined;

    /// Depth attachment format, or eUndefined for no depth buffer.
    vk::Format depthFormat = vk::Format::eUndefined;

    /// How vertex memory is laid out. Empty means the vertex shader builds its
    /// own vertices from gl_VertexIndex and no buffer is bound.
    std::span<const vk::VertexInputBindingDescription> vertexBindings;
    std::span<const vk::VertexInputAttributeDescription> vertexAttributes;

    /// Descriptor set layouts, in set order: index 0 becomes set 0 in GLSL.
    std::span<const vk::DescriptorSetLayout> setLayouts;

    /// Push constants: a small block (128 bytes guaranteed) written straight
    /// into the command buffer, with no descriptor set or buffer involved.
    u32 pushConstantSize = 0;
    vk::ShaderStageFlags pushConstantStages{};

    vk::CullModeFlags cullMode = vk::CullModeFlagBits::eBack;

    /// eFill for solid triangles, eLine to rasterise their edges instead.
    /// eLine needs the fillModeNonSolid device feature.
    vk::PolygonMode polygonMode = vk::PolygonMode::eFill;

    /// Line thickness. Anything above 1.0 needs the wideLines device feature.
    f32 lineWidth = 1.0f;

    /// Nudges generated depth values towards the camera.
    ///
    /// A wireframe drawn over the same geometry produces identical depth
    /// values, and the test then resolves them arbitrarily per pixel - the
    /// lines flicker in and out. A small negative bias pulls them in front.
    f32 depthBiasConstant = 0.0f;

    /// Which winding counts as front-facing, measured in FRAMEBUFFER space.
    ///
    /// eCounterClockwise matches the glTF convention fumar follows, and it
    /// survives the trip to the framebuffer because TWO flips cancel out:
    /// Vulkan's framebuffer has Y pointing down, which on its own would reverse
    /// the winding, and fumar's perspective matrix negates Y (see
    /// core/math/mat.hpp), which reverses it back.
    ///
    /// Get this wrong and nothing errors - the front faces are culled and the
    /// back ones survive, so a cube renders as an open box seen from inside and
    /// a ground plane disappears entirely.
    vk::FrontFace frontFace = vk::FrontFace::eCounterClockwise;

    bool depthTest = true;
    bool depthWrite = true;

    /// eLess keeps whatever is closer to the camera. eGreater belongs with a
    /// reversed-Z projection, which trades nothing for far better depth
    /// precision - worth doing later, but it has to change in both places.
    vk::CompareOp depthCompare = vk::CompareOp::eLess;
};

/// A compiled graphics pipeline plus the layout it was built with.
///
/// In Vulkan nearly all render state - shaders, blending, culling, vertex
/// layout - is frozen into an immutable object at creation time. That is what
/// makes draw calls cheap, and why changing state means switching pipelines
/// rather than calling a setter.
class GraphicsPipeline {
public:
    GraphicsPipeline(Device& device, const GraphicsPipelineDesc& desc);

    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

    vk::Pipeline handle() const { return *m_pipeline; }

    vk::PipelineLayout layout() const { return *m_layout; }

private:
    // The layout outlives the pipeline: vkCmdPushConstants and
    // vkCmdBindDescriptorSets both take it, so it is still needed while
    // recording. Declaration order means the pipeline is destroyed first.
    vk::UniquePipelineLayout m_layout;
    vk::UniquePipeline m_pipeline;
};

} // namespace fumar::rhi
