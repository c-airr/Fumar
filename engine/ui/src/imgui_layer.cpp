#include "fumar/ui/imgui_layer.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/core/log.hpp"
#include "fumar/platform/paths.hpp"
#include "fumar/platform/window.hpp"
#include "fumar/render/renderer.hpp"
#include "fumar/rhi/device.hpp"
#include "fumar/rhi/instance.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <array>
#include <string>

// SDL_Event is needed to hand events to ImGui's backend, and nothing else here
// touches SDL.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_events.h>

namespace fumar {
namespace {

void checkVkResult(VkResult result) {
    if (result != VK_SUCCESS) {
        FUMAR_ERROR("ImGui Vulkan backend reported {}", vk::to_string(static_cast<vk::Result>(result)));
    }
}

/// Bridges ImGui's function loader to the dispatcher fumar already populated.
///
/// The backend is compiled with IMGUI_IMPL_VULKAN_NO_PROTOTYPES because we
/// never link vulkan-1, so it has no entry points of its own. Resolving through
/// the instance means it sees exactly the same driver we do.
PFN_vkVoidFunction loadVulkanFunction(const char* name, void* userData) {
    auto instance = static_cast<VkInstance>(userData);
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(instance, name);
}

void applyStyle() {
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.SeparatorTextBorderSize = 1.0f;

    // Slightly cooler and darker than the default, so the viewport rather than
    // the surrounding panels is what the eye lands on.
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.07f, 0.08f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.17f, 0.21f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.23f, 0.29f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.18f, 0.22f, 0.30f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.30f, 0.40f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.28f, 0.36f, 0.48f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.18f, 0.22f, 0.30f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.11f, 0.13f, 0.17f, 1.00f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.20f, 0.26f, 0.36f, 1.00f);
}

} // namespace

ImGuiLayer::ImGuiLayer(Window& window, Renderer& renderer) : m_renderer(renderer) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Docking is what makes this an editor rather than a window with overlays:
    // panels can be dragged, split and tabbed against each other.
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Multi-viewport (panels torn off into real OS windows) is deliberately not
    // enabled: it needs the backend to create and present its own swapchains,
    // which is a second renderer's worth of code for a convenience.

    // Editor layout is worth keeping between runs; it is written next to the
    // executable rather than the working directory.
    static std::string iniPath;
    iniPath = (executableDirectory() / "fumar_editor.ini").string();
    io.IniFilename = iniPath.c_str();

    applyStyle();

    ImGui_ImplSDL3_InitForVulkan(window.handle());

    // ImGui allocates one descriptor set per texture handed to it. Since 1.92
    // it also keeps a separate sampler descriptor, so the pool has to offer
    // both types - a pool missing one is a validation warning at init and an
    // out-of-pool-memory failure on drivers that report it properly.
    const std::array<vk::DescriptorPoolSize, 3> poolSizes{
        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, 64},
        vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 64},
        vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 64},
    };
    m_descriptorPool = renderer.device().handle().createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo{
        // ImGui frees individual sets when a texture goes away, which requires
        // this flag - without it the pool can only be reset wholesale.
        .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
        .maxSets = 64,
        .poolSizeCount = static_cast<u32>(poolSizes.size()),
        .pPoolSizes = poolSizes.data(),
    });

    const VkInstance instance = renderer.instance().handle();
    ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, loadVulkanFunction, instance);

    // Dynamic rendering again, matching how the scene is drawn: no render pass
    // object exists to hand over, only the attachment format.
    const VkFormat colorFormat = static_cast<VkFormat>(renderer.swapchainFormat());

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VK_API_VERSION_1_3;
    initInfo.Instance = instance;
    initInfo.PhysicalDevice = renderer.device().physicalDevice();
    initInfo.Device = renderer.device().handle();
    initInfo.QueueFamily = renderer.device().queueFamilies().graphics;
    initInfo.Queue = renderer.device().graphicsQueue();
    initInfo.DescriptorPool = *m_descriptorPool;
    initInfo.MinImageCount = 2;
    initInfo.ImageCount = renderer.swapchainImageCount();
    initInfo.CheckVkResultFn = checkVkResult;
    initInfo.UseDynamicRendering = true;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat;

    // The depth format has to be declared even though the interface never reads
    // or writes depth: the pipeline is used inside a render pass that HAS a
    // depth attachment, and Vulkan requires the declaration to match the pass
    // exactly. Leaving it undefined is a validation error on every draw.
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat =
        static_cast<VkFormat>(renderer.depthFormat());

    FUMAR_VERIFY_MSG(ImGui_ImplVulkan_Init(&initInfo), "ImGui Vulkan backend failed to initialise");

    // Every OS event reaches ImGui through the window's hook.
    window.setEventHook([](const void* event) {
        ImGui_ImplSDL3_ProcessEvent(static_cast<const SDL_Event*>(event));
    });

    FUMAR_INFO("imgui {} ready, docking enabled", IMGUI_VERSION);
}

ImGuiLayer::~ImGuiLayer() {
    // The backend holds device objects, so the GPU must be finished with them.
    m_renderer.device().waitIdle();

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    FUMAR_INFO("imgui shut down");
}

void ImGuiLayer::beginFrame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::endFrame() {
    ImGui::Render();
}

void ImGuiLayer::record(vk::CommandBuffer cmd) {
    ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData != nullptr) {
        ImGui_ImplVulkan_RenderDrawData(drawData, cmd);
    }
}

bool ImGuiLayer::wantsMouse() const {
    return ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiLayer::wantsKeyboard() const {
    return ImGui::GetIO().WantCaptureKeyboard;
}

} // namespace fumar
