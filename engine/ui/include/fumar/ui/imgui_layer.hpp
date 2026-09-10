#pragma once

#include "fumar/core/types.hpp"
#include "fumar/rhi/vk_common.hpp"

#include <imgui.h>

namespace fumar {

class Renderer;
class Window;

/// Wires Dear ImGui into fumar's window and renderer.
///
/// ImGui is immediate mode: there are no widget objects to create and keep in
/// sync with the data behind them. Every frame the whole interface is described
/// again from scratch, which is exactly the right model for an editor, where
/// what is on screen is a direct function of the scene's current state.
///
/// Lives in its own module rather than in the renderer, because the renderer
/// has no business knowing what a window or a button is - it only accepts a
/// callback that records extra draw commands.
/// An interface colour, converted from the sRGB values a colour picker shows
/// into what the sRGB swapchain needs.
///
/// Every colour the editor names has to go through this. The window is
/// presented in an _SRGB format, so the hardware applies the sRGB curve to
/// whatever is written: pass 0.086 straight through and 0.33 appears on screen.
/// The theme itself is converted wholesale inside the layer; this is for the
/// handful of colours panels name directly.
ImVec4 uiColor(f32 red, f32 green, f32 blue, f32 alpha = 1.0f);

class ImGuiLayer {
public:
    ImGuiLayer(Window& window, Renderer& renderer);
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    /// Starts a UI frame. Widget calls go between this and endFrame().
    void beginFrame();

    /// Finishes the frame and builds the draw lists.
    ///
    /// Separate from recording, because the renderer may skip a frame entirely
    /// (a minimised window, a stale swapchain) and ImGui still has to see a
    /// matched NewFrame/Render pair or it asserts on the next one.
    void endFrame();

    /// Records the UI draw commands. Install with Renderer::setOverlay.
    void record(vk::CommandBuffer cmd);

    /// Makes a Vulkan image displayable by ImGui::Image.
    ///
    /// Behind the id is a descriptor set the backend allocates and keeps. It
    /// stays valid until unregisterTexture, so the caller must re-register
    /// whenever the underlying image is recreated - a resized viewport, for
    /// instance - or ImGui keeps sampling a destroyed image.
    ImTextureID registerTexture(vk::ImageView view, vk::Sampler sampler);

    void unregisterTexture(ImTextureID id);

    /// True while ImGui wants the mouse - hovering a panel, dragging a slider.
    /// The camera checks this so clicking in a panel does not also swing the
    /// view behind it.
    bool wantsMouse() const;

    /// True while ImGui wants the keyboard, typically because a text field has
    /// focus. WASD must not move the camera then.
    bool wantsKeyboard() const;

private:
    Renderer& m_renderer;

    /// ImGui allocates a descriptor set per texture it is asked to display, so
    /// it gets a pool of its own rather than competing with the material budget.
    vk::UniqueDescriptorPool m_descriptorPool;
};

} // namespace fumar
