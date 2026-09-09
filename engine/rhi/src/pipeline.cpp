#include "fumar/rhi/pipeline.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/core/file.hpp"
#include "fumar/core/log.hpp"
#include "fumar/rhi/device.hpp"

#include <array>
#include <utility>

namespace fumar::rhi {
namespace {

vk::UniqueShaderModule loadShaderModule(vk::Device device, const std::filesystem::path& path) {
    const auto code = readSpirvFile(path);
    FUMAR_VERIFY_MSG(code.has_value(), "could not load shader '{}'", path.string());

    return device.createShaderModuleUnique(vk::ShaderModuleCreateInfo{
        // codeSize is in BYTES while pCode is in words - one of the few places
        // in the API where the two units sit next to each other.
        .codeSize = code->size() * sizeof(u32),
        .pCode = code->data(),
    });
}

} // namespace

GraphicsPipeline::GraphicsPipeline(Device& device, const GraphicsPipelineDesc& desc) {
    const vk::Device handle = device.handle();

    const vk::UniqueShaderModule vertexModule = loadShaderModule(handle, desc.vertexShader);
    const vk::UniqueShaderModule fragmentModule = loadShaderModule(handle, desc.fragmentShader);

    const std::array<vk::PipelineShaderStageCreateInfo, 2> stages{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = *vertexModule,
            .pName = "main", // the entry point inside the SPIR-V module
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = *fragmentModule,
            .pName = "main",
        },
    };

    // --- layout -------------------------------------------------------------
    const vk::PushConstantRange pushConstantRange{
        .stageFlags = desc.pushConstantStages,
        .offset = 0,
        .size = desc.pushConstantSize,
    };

    const bool hasPushConstants = desc.pushConstantSize > 0;
    m_layout = handle.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo{
        .setLayoutCount = static_cast<u32>(desc.setLayouts.size()),
        .pSetLayouts = desc.setLayouts.data(),
        .pushConstantRangeCount = hasPushConstants ? 1u : 0u,
        .pPushConstantRanges = hasPushConstants ? &pushConstantRange : nullptr,
    });

    // --- fixed function state -----------------------------------------------
    const vk::PipelineVertexInputStateCreateInfo vertexInput{
        .vertexBindingDescriptionCount = static_cast<u32>(desc.vertexBindings.size()),
        .pVertexBindingDescriptions = desc.vertexBindings.data(),
        .vertexAttributeDescriptionCount = static_cast<u32>(desc.vertexAttributes.size()),
        .pVertexAttributeDescriptions = desc.vertexAttributes.data(),
    };

    const vk::PipelineInputAssemblyStateCreateInfo inputAssembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
        .primitiveRestartEnable = VK_FALSE,
    };

    // The counts must be right even though the values come from dynamic state
    // later - the pipeline still needs to know there is exactly one of each.
    const vk::PipelineViewportStateCreateInfo viewportState{
        .viewportCount = 1,
        .scissorCount = 1,
    };

    const vk::PipelineRasterizationStateCreateInfo rasterization{
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = desc.cullMode,
        .frontFace = desc.frontFace,
        .depthBiasEnable = VK_FALSE,
        // Not optional: a line width of 0 is invalid and the validation layers
        // will say so, even when nothing draws lines.
        .lineWidth = 1.0f,
    };

    const vk::PipelineMultisampleStateCreateInfo multisample{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
        .sampleShadingEnable = VK_FALSE,
    };

    const bool hasDepth = desc.depthFormat != vk::Format::eUndefined;
    const vk::PipelineDepthStencilStateCreateInfo depthStencil{
        .depthTestEnable = (hasDepth && desc.depthTest) ? VK_TRUE : VK_FALSE,
        // Test and write are separate on purpose: transparent geometry is
        // usually tested against the depth buffer but must not write into it,
        // or it would occlude the transparent surfaces drawn after it.
        .depthWriteEnable = (hasDepth && desc.depthWrite) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = desc.depthCompare,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
        .minDepthBounds = 0.0f,
        .maxDepthBounds = 1.0f,
    };

    const vk::PipelineColorBlendAttachmentState blendAttachment{
        .blendEnable = VK_FALSE,
        // Easy to miss and silently fatal: the default mask is zero, which
        // writes no channels at all and produces a blank image with no error.
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };

    const vk::PipelineColorBlendStateCreateInfo colorBlend{
        .logicOpEnable = VK_FALSE,
        .attachmentCount = 1,
        .pAttachments = &blendAttachment,
    };

    // Viewport and scissor are dynamic, so a window resize does not require
    // rebuilding the pipeline - only re-recording the command buffer.
    const std::array<vk::DynamicState, 2> dynamicStates{
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };

    const vk::PipelineDynamicStateCreateInfo dynamicState{
        .dynamicStateCount = static_cast<u32>(dynamicStates.size()),
        .pDynamicStates = dynamicStates.data(),
    };

    // --- dynamic rendering --------------------------------------------------
    // Replaces VkRenderPass and VkFramebuffer entirely. Instead of declaring
    // attachments up front and building framebuffer objects for them, the
    // pipeline just states which formats it writes and rendering begins with
    // vkCmdBeginRendering.
    const vk::Format colorFormat = desc.colorFormat;
    const vk::PipelineRenderingCreateInfo renderingInfo{
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &colorFormat,
        .depthAttachmentFormat = desc.depthFormat,
    };

    const vk::GraphicsPipelineCreateInfo pipelineInfo{
        .pNext = &renderingInfo,
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = hasDepth ? &depthStencil : nullptr,
        .pColorBlendState = &colorBlend,
        .pDynamicState = &dynamicState,
        .layout = *m_layout,
        // Null, because dynamic rendering is in use.
        .renderPass = nullptr,
    };

    // A pipeline cache would go in the first argument; without one every
    // pipeline is compiled from scratch at startup.
    auto result = handle.createGraphicsPipelineUnique(nullptr, pipelineInfo);
    FUMAR_VERIFY_MSG(result.result == vk::Result::eSuccess, "pipeline creation failed: {}",
                     vk::to_string(result.result));
    m_pipeline = std::move(result.value);

    FUMAR_INFO("graphics pipeline built from '{}' and '{}'", desc.vertexShader.filename().string(),
               desc.fragmentShader.filename().string());
}

} // namespace fumar::rhi
