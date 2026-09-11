#pragma once

#include "fumar/core/math/vec.hpp"
#include "fumar/core/types.hpp"

// Only the Vulkan handle types and PFN_vkGetInstanceProcAddr are needed here.
// vulkan_core.h is the platform-neutral part of the API, so including it does
// not drag in windows.h or Xlib.
#include <vulkan/vulkan_core.h>

#include <functional>
#include <string>
#include <vector>

// Forward declaration instead of including SDL: nothing outside this module
// should be able to reach for SDL functions, which is what makes swapping the
// backend later a local change rather than a rewrite.
struct SDL_Window;

namespace fumar {

/// Keys the engine cares about, kept as our own enum so SDL scancodes do not
/// leak into gameplay or editor code.
enum class Key : u8 {
    W,
    A,
    S,
    D,
    Q,
    E,
    Space,
    LeftShift,
    LeftControl,
    Escape,
    Up,
    Down,
    Left,
    Right,
    Count,
};

enum class MouseButton : u8 {
    Left,
    Right,
    Middle,
    Count,
};

struct WindowDesc {
    std::string title = "fumar";
    u32 width = 1280;
    u32 height = 720;
    bool resizable = true;
};

/// An OS window plus its event stream.
///
/// Owns the underlying SDL window and releases it in the destructor, so there
/// is no explicit shutdown call to forget. SDL itself is initialised on the
/// first window and shut down with the last one.
class Window {
public:
    explicit Window(const WindowDesc& desc);
    ~Window();

    // A window owns a unique OS resource, so copying it makes no sense.
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Moving is fine: the handle transfers and the source is left empty.
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;

    /// Drains the OS event queue. Call once per frame - a window that stops
    /// pumping events is what the system reports as "not responding".
    void pumpEvents();

    /// Called for every OS event before the window acts on it.
    ///
    /// The argument is an `SDL_Event*`, passed as void* so this header stays
    /// free of SDL. That is a deliberate leak of one implementation detail:
    /// ImGui's input backend takes SDL events directly, and inventing a
    /// translation layer for every event type would be a lot of code that only
    /// ImGui would ever read.
    using EventHook = std::function<void(const void*)>;
    void setEventHook(EventHook hook) { m_eventHook = std::move(hook); }

    /// Blocks until an event arrives or the timeout expires, then handles the
    /// queue as pumpEvents() does. Used while minimised, where there is nothing
    /// to draw and spinning would burn a core for nothing.
    void waitEvents(u32 timeoutMs);

    /// Files dropped onto the window since the last call, and clears the list.
    ///
    /// Queued rather than delivered through a callback, because dropping a file
    /// usually means loading it, and loading it means rebuilding the scene -
    /// which must not happen in the middle of pumping the event queue.
    std::vector<std::string> consumeDroppedFiles();

    /// True while the key is held. Level-triggered, which is what continuous
    /// movement wants; a key press as a one-off event would need its own edge
    /// detection.
    bool keyDown(Key key) const;

    bool mouseButtonDown(MouseButton button) const;

    /// Mouse movement accumulated during the last pumpEvents(), in pixels.
    ///
    /// Read as a delta rather than an absolute position because a look control
    /// cares about how far the mouse moved, and in relative mode there is no
    /// meaningful absolute position anyway - the cursor is pinned.
    Vec2 mouseDelta() const { return m_mouseDelta; }

    /// Hides the cursor and frees it from the screen edges, so looking around
    /// never runs out of desk. This is what every first-person camera needs.
    /// No-ops when the window lacks keyboard focus - SDL would otherwise latch
    /// a sticky per-window flag without actually capturing the pointer.
    void setRelativeMouse(bool enabled);

    /// True only while the pointer is actually captured, not merely requested.
    bool relativeMouse() const;

    /// Whether this window is the one receiving keyboard input.
    ///
    /// Worth checking before acting on any key or button: the OS keeps
    /// reporting the physical state of the mouse and keyboard whether or not
    /// this window is in front, so an editor that does not ask will happily fly
    /// its camera while you are typing in another application.
    bool hasFocus() const;

    bool shouldClose() const { return m_shouldClose; }

    /// A minimised window has a zero-sized framebuffer, which Vulkan rejects,
    /// so the render loop skips drawing while this is true.
    bool minimized() const { return m_minimized; }

    /// Returns true once after the window has been resized, then clears the
    /// flag. Consuming it here keeps the "swapchain is stale" decision in one
    /// place instead of spread across the renderer.
    bool consumeResized();

    /// Drawable size in PIXELS, which on a display with scaling is not the same
    /// as the window size in logical points. The swapchain needs pixels.
    Extent2D framebufferSize() const;

    /// Instance extensions the windowing system requires (VK_KHR_surface plus
    /// one platform-specific extension). Must be passed to vkCreateInstance or
    /// surface creation will fail.
    std::vector<const char*> requiredVulkanExtensions() const;

    /// The one Vulkan entry point that is resolved without an instance.
    /// Everything else in the API is loaded through it, so handing this to the
    /// RHI is all it takes to bootstrap the loader - no vulkan-1 library to
    /// link, and no second loader fighting with SDL's.
    PFN_vkGetInstanceProcAddr vulkanLoaderFunction() const;

    /// Creates the surface this window presents to. Ownership stays with the
    /// caller, who must pass it back to destroySurface().
    VkSurfaceKHR createSurface(VkInstance instance) const;

    void destroySurface(VkInstance instance, VkSurfaceKHR surface) const;

    SDL_Window* handle() const { return m_window; }

private:
    SDL_Window* m_window = nullptr;
    bool m_shouldClose = false;
    bool m_minimized = false;
    bool m_resized = false;
    Vec2 m_mouseDelta;
    EventHook m_eventHook;
    std::vector<std::string> m_droppedFiles;
};

} // namespace fumar
