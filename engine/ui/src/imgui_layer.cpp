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
#include <cmath>
#include <filesystem>
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

/// The editor accent: a burnt orange, used for borders, selection and anything
/// the eye is meant to land on. Kept in one place because it appears in the
/// interface and in the scene shader, and the two have to agree.
constexpr ImVec4 kAccent{0.659f, 0.373f, 0.173f, 1.00f};

ImVec4 withAlpha(const ImVec4& colour, float alpha) {
    return ImVec4(colour.x, colour.y, colour.z, alpha);
}

ImVec4 scaled(const ImVec4& colour, float factor) {
    return ImVec4(colour.x * factor, colour.y * factor, colour.z * factor, colour.w);
}

/// One channel from sRGB to linear, the exact piecewise transfer function
/// rather than the pow(2.2) approximation - the two differ most in the dark
/// values, which is precisely where an almost-black interface lives.
f32 srgbChannelToLinear(f32 value) {
    return value <= 0.04045f ? value / 12.92f
                             : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

/// Why every interface colour goes through this.
///
/// The window is presented through a swapchain in an _SRGB format, which means
/// the hardware applies the sRGB transfer function to whatever a shader writes.
/// That is exactly right for the 3D scene, which is rendered in linear light
/// and tone mapped - but interface colours are picked by eye, in the sRGB
/// numbers a colour picker shows. Writing 0.086 straight out and letting the
/// hardware encode it produces 0.33 on screen: the near-black theme comes out
/// mid-grey and the whole editor looks washed out.
///
/// Converting here undoes that, so the number written in the palette is the
/// number that appears. Alpha is not a colour and is left alone.
ImVec4 toLinear(const ImVec4& srgb) {
    return ImVec4(srgbChannelToLinear(srgb.x), srgbChannelToLinear(srgb.y),
                  srgbChannelToLinear(srgb.z), srgb.w);
}

/// Loads the interface font.
///
/// Dear ImGui's built-in font is a 13-pixel bitmap from 2005. It is there so
/// that a first run shows SOMETHING, not because anyone should ship it. Roboto
/// comes from ImGui's own misc/fonts directory - already on disk as part of the
/// dependency - and is copied beside the executable at build time.
///
/// Both loads are allowed to fail: a missing font falls back to the built-in
/// one rather than refusing to start.
void loadFonts(ImGuiIO& io, const std::filesystem::path& fontDirectory) {
    const auto load = [&](const char* file, f32 size) -> ImFont* {
        const std::filesystem::path path = fontDirectory / file;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            FUMAR_WARN("font '{}' not found, falling back to the built-in one", path.string());
            return nullptr;
        }
        return io.Fonts->AddFontFromFileTTF(path.string().c_str(), size);
    };

    if (ImFont* ui = load("Roboto-Medium.ttf", 16.0f)) {
        io.FontDefault = ui;
    }

    // A second face, monospaced, for anything where columns have to line up:
    // script source, numbers in the statistics panel. Registered here so panels
    // can reach it through io.Fonts->Fonts[1] without loading it themselves.
    load("Cousine-Regular.ttf", 15.0f);
}

} // namespace

ImVec4 uiColor(f32 red, f32 green, f32 blue, f32 alpha) {
    return ImVec4(srgbChannelToLinear(red), srgbChannelToLinear(green), srgbChannelToLinear(blue),
                  alpha);
}

namespace {

void applyStyle() {
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();

    // Rounded, with a visible border on every framed control. The border is
    // what carries the accent colour - filling controls with it instead would
    // be loud and would leave nothing to highlight the active one with.
    style.WindowRounding = 5.0f;
    style.ChildRounding = 5.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 5.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 5.0f;
    style.ScrollbarRounding = 6.0f;

    style.FrameBorderSize = 1.0f;
    style.WindowBorderSize = 1.0f;
    style.TabBarBorderSize = 2.0f;

    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 12.0f;
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.SeparatorTextBorderSize = 1.0f;
    style.SeparatorTextPadding = ImVec2(16.0f, 4.0f);

    // Near-black rather than black: a true black background makes every panel
    // edge disappear and leaves nothing for shadows to sit against.
    const ImVec4 base{0.086f, 0.086f, 0.094f, 1.00f};
    const ImVec4 raised{0.125f, 0.125f, 0.137f, 1.00f};
    const ImVec4 sunken{0.055f, 0.055f, 0.063f, 1.00f};

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = base;
    colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_PopupBg] = raised;
    colors[ImGuiCol_MenuBarBg] = sunken;

    // The accent, dimmed. A full-strength border on every control would compete
    // with the selection highlight, which is the same colour at full strength.
    colors[ImGuiCol_Border] = withAlpha(kAccent, 0.55f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    colors[ImGuiCol_FrameBg] = raised;
    colors[ImGuiCol_FrameBgHovered] = scaled(kAccent, 0.35f);
    colors[ImGuiCol_FrameBgActive] = scaled(kAccent, 0.55f);

    colors[ImGuiCol_TitleBg] = sunken;
    colors[ImGuiCol_TitleBgActive] = raised;
    colors[ImGuiCol_TitleBgCollapsed] = sunken;

    colors[ImGuiCol_Button] = raised;
    colors[ImGuiCol_ButtonHovered] = scaled(kAccent, 0.55f);
    colors[ImGuiCol_ButtonActive] = kAccent;

    colors[ImGuiCol_Header] = scaled(kAccent, 0.40f);
    colors[ImGuiCol_HeaderHovered] = scaled(kAccent, 0.60f);
    colors[ImGuiCol_HeaderActive] = kAccent;

    colors[ImGuiCol_Tab] = sunken;
    colors[ImGuiCol_TabHovered] = scaled(kAccent, 0.60f);
    colors[ImGuiCol_TabSelected] = raised;
    // The bright line under the active tab, which is where the accent reads
    // most clearly.
    colors[ImGuiCol_TabSelectedOverline] = kAccent;
    colors[ImGuiCol_TabDimmed] = sunken;
    colors[ImGuiCol_TabDimmedSelected] = base;
    colors[ImGuiCol_TabDimmedSelectedOverline] = withAlpha(kAccent, 0.35f);

    colors[ImGuiCol_CheckMark] = kAccent;
    colors[ImGuiCol_SliderGrab] = scaled(kAccent, 0.80f);
    colors[ImGuiCol_SliderGrabActive] = kAccent;
    colors[ImGuiCol_ResizeGrip] = withAlpha(kAccent, 0.25f);
    colors[ImGuiCol_ResizeGripHovered] = withAlpha(kAccent, 0.60f);
    colors[ImGuiCol_ResizeGripActive] = kAccent;

    colors[ImGuiCol_Separator] = withAlpha(kAccent, 0.35f);
    colors[ImGuiCol_SeparatorHovered] = withAlpha(kAccent, 0.65f);
    colors[ImGuiCol_SeparatorActive] = kAccent;

    colors[ImGuiCol_DockingPreview] = withAlpha(kAccent, 0.60f);
    colors[ImGuiCol_DockingEmptyBg] = sunken;

    colors[ImGuiCol_ScrollbarBg] = sunken;
    colors[ImGuiCol_ScrollbarGrab] = raised;
    colors[ImGuiCol_ScrollbarGrabHovered] = scaled(kAccent, 0.55f);
    colors[ImGuiCol_ScrollbarGrabActive] = kAccent;

    colors[ImGuiCol_Text] = ImVec4(0.88f, 0.88f, 0.89f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.45f, 0.47f, 1.00f);
    colors[ImGuiCol_TextSelectedBg] = withAlpha(kAccent, 0.45f);

    colors[ImGuiCol_NavCursor] = kAccent;
    colors[ImGuiCol_DragDropTarget] = kAccent;

    // Last, and applied to everything at once: see toLinear above. Doing it
    // here rather than at each assignment keeps the palette readable as the
    // sRGB values it was designed in.
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        colors[i] = toLinear(colors[i]);
    }
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

    loadFonts(io, executableDirectory() / "fonts");
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

ImTextureID ImGuiLayer::registerTexture(vk::ImageView view, vk::Sampler sampler) {
    // The layout is what the image will be in when ImGui samples it, which the
    // renderer guarantees with a barrier at the end of the scene pass.
    const VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
        sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return reinterpret_cast<ImTextureID>(set);
}

void ImGuiLayer::unregisterTexture(ImTextureID id) {
    if (id == 0) {
        return;
    }
    // Frees the descriptor set back to the pool, which is why the pool was
    // created with eFreeDescriptorSet.
    ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(id));
}

bool ImGuiLayer::wantsMouse() const {
    return ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiLayer::wantsKeyboard() const {
    return ImGui::GetIO().WantCaptureKeyboard;
}

} // namespace fumar
