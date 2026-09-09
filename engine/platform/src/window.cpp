#include "fumar/platform/window.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/core/log.hpp"

// SDL normally #defines main to SDL_main so it can run its own startup code
// first. We do not want that - fumar owns its entry point - so we tell SDL the
// main function is already handled and call SDL_SetMainReady() ourselves.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>

#include <utility>

namespace fumar {
namespace {

/// SDL is a global library with a single init/quit pair, but a Window is an
/// ordinary object that can be created and destroyed at any time. This counter
/// bridges the two: the first window brings SDL up, the last one takes it down.
int g_liveWindows = 0;

void acquireSdl() {
    if (g_liveWindows == 0) {
        SDL_SetMainReady();
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            FUMAR_FATAL("SDL_Init failed: {}", SDL_GetError());
            FUMAR_DEBUG_BREAK();
        }
        FUMAR_INFO("SDL {} initialised, video driver '{}'", SDL_GetRevision(), SDL_GetCurrentVideoDriver());
    }
    ++g_liveWindows;
}

void releaseSdl() {
    --g_liveWindows;
    if (g_liveWindows == 0) {
        SDL_Quit();
        FUMAR_DEBUG("SDL shut down");
    }
}

/// Our Key enum to SDL scancodes.
///
/// Scancodes describe the physical key position rather than the letter printed
/// on it, so W is the same physical key on QWERTY and AZERTY. For movement that
/// is what you want; for text entry you would want keycodes instead.
SDL_Scancode toScancode(Key key) {
    switch (key) {
    case Key::W: return SDL_SCANCODE_W;
    case Key::A: return SDL_SCANCODE_A;
    case Key::S: return SDL_SCANCODE_S;
    case Key::D: return SDL_SCANCODE_D;
    case Key::Q: return SDL_SCANCODE_Q;
    case Key::E: return SDL_SCANCODE_E;
    case Key::Space: return SDL_SCANCODE_SPACE;
    case Key::LeftShift: return SDL_SCANCODE_LSHIFT;
    case Key::LeftControl: return SDL_SCANCODE_LCTRL;
    case Key::Escape: return SDL_SCANCODE_ESCAPE;
    case Key::Count: break;
    }
    return SDL_SCANCODE_UNKNOWN;
}

Uint32 toButtonMask(MouseButton button) {
    switch (button) {
    case MouseButton::Left: return SDL_BUTTON_MASK(SDL_BUTTON_LEFT);
    case MouseButton::Right: return SDL_BUTTON_MASK(SDL_BUTTON_RIGHT);
    case MouseButton::Middle: return SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE);
    case MouseButton::Count: break;
    }
    return 0;
}

} // namespace

Window::Window(const WindowDesc& desc) {
    acquireSdl();

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
    if (desc.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }

    m_window = SDL_CreateWindow(desc.title.c_str(), static_cast<int>(desc.width),
                                static_cast<int>(desc.height), flags);
    if (m_window == nullptr) {
        FUMAR_FATAL("SDL_CreateWindow failed: {}", SDL_GetError());
        releaseSdl();
        FUMAR_DEBUG_BREAK();
        return;
    }

    const Extent2D size = framebufferSize();
    FUMAR_INFO("window '{}' created, {}x{} px", desc.title, size.width, size.height);
}

Window::~Window() {
    if (m_window != nullptr) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
        releaseSdl();
    }
}

Window::Window(Window&& other) noexcept
    : m_window(std::exchange(other.m_window, nullptr)),
      m_shouldClose(other.m_shouldClose),
      m_minimized(other.m_minimized),
      m_resized(other.m_resized) {
    // The moved-from window no longer owns anything, so its destructor must not
    // decrement the SDL reference count - hence the exchange above.
}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        if (m_window != nullptr) {
            SDL_DestroyWindow(m_window);
            releaseSdl();
        }
        m_window = std::exchange(other.m_window, nullptr);
        m_shouldClose = other.m_shouldClose;
        m_minimized = other.m_minimized;
        m_resized = other.m_resized;
    }
    return *this;
}

void Window::pumpEvents() {
    // The delta covers one frame, so it starts at zero every time and
    // accumulates whatever motion events arrive below.
    m_mouseDelta = Vec2{};

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // The hook sees the event first, so a UI layer can record it. It does
        // not get to swallow it: whether the UI wants keyboard or mouse focus
        // is a question the UI answers separately, and hiding events here would
        // leave the window unable to notice a resize or a close request.
        if (m_eventHook) {
            m_eventHook(&event);
        }

        switch (event.type) {
        case SDL_EVENT_QUIT:
            m_shouldClose = true;
            break;

        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            m_shouldClose = true;
            break;

        // Fires when the drawable size changes, including on a DPI change,
        // which plain SDL_EVENT_WINDOW_RESIZED can miss.
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            m_resized = true;
            break;

        case SDL_EVENT_WINDOW_MINIMIZED:
            m_minimized = true;
            break;

        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
            m_minimized = false;
            m_resized = true;
            break;

        case SDL_EVENT_MOUSE_MOTION:
            // Several motion events can arrive per frame; summing them keeps
            // fast mouse movement from being clipped to the last one.
            m_mouseDelta.x += event.motion.xrel;
            m_mouseDelta.y += event.motion.yrel;
            break;

        case SDL_EVENT_KEY_DOWN:
            if (event.key.key == SDLK_ESCAPE) {
                // Escape releases the mouse first, and only closes the window
                // if it was already free - otherwise a captured cursor would be
                // impossible to escape without quitting.
                if (relativeMouse()) {
                    setRelativeMouse(false);
                } else {
                    m_shouldClose = true;
                }
            }
            break;

        default:
            break;
        }
    }
}

void Window::waitEvents(u32 timeoutMs) {
    SDL_Event event;
    // Blocks for up to the timeout waiting for the first event, then the usual
    // drain picks up anything else that has queued behind it.
    if (SDL_WaitEventTimeout(&event, static_cast<Sint32>(timeoutMs))) {
        SDL_PushEvent(&event);
    }
    pumpEvents();
}

bool Window::keyDown(Key key) const {
    // SDL owns this array and keeps it updated as events are pumped, so there
    // is no per-key state for us to track.
    const bool* keyboard = SDL_GetKeyboardState(nullptr);
    if (keyboard == nullptr) {
        return false;
    }
    const SDL_Scancode scancode = toScancode(key);
    return scancode != SDL_SCANCODE_UNKNOWN && keyboard[scancode];
}

bool Window::mouseButtonDown(MouseButton button) const {
    const SDL_MouseButtonFlags state = SDL_GetMouseState(nullptr, nullptr);
    return (state & toButtonMask(button)) != 0;
}

void Window::setRelativeMouse(bool enabled) {
    if (m_window == nullptr) {
        return;
    }
    if (!SDL_SetWindowRelativeMouseMode(m_window, enabled)) {
        FUMAR_WARN("SDL_SetWindowRelativeMouseMode failed: {}", SDL_GetError());
    }
}

bool Window::relativeMouse() const {
    return m_window != nullptr && SDL_GetWindowRelativeMouseMode(m_window);
}

bool Window::consumeResized() {
    return std::exchange(m_resized, false);
}

Extent2D Window::framebufferSize() const {
    if (m_window == nullptr) {
        return {};
    }

    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSizeInPixels(m_window, &width, &height)) {
        FUMAR_WARN("SDL_GetWindowSizeInPixels failed: {}", SDL_GetError());
        return {};
    }

    // A minimised window reports 0x0; the negative case should not happen, but
    // clamping here means callers never have to think about it.
    return Extent2D{
        width > 0 ? static_cast<u32>(width) : 0u,
        height > 0 ? static_cast<u32>(height) : 0u,
    };
}

std::vector<const char*> Window::requiredVulkanExtensions() const {
    Uint32 count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (extensions == nullptr) {
        FUMAR_ERROR("SDL_Vulkan_GetInstanceExtensions failed: {}", SDL_GetError());
        return {};
    }
    return std::vector<const char*>(extensions, extensions + count);
}

PFN_vkGetInstanceProcAddr Window::vulkanLoaderFunction() const {
    // SDL hands back a generic function pointer; casting between function
    // pointer types is well defined as long as it is called through the right
    // signature, which it is.
    auto* loader = SDL_Vulkan_GetVkGetInstanceProcAddr();
    FUMAR_VERIFY_MSG(loader != nullptr, "SDL could not resolve vkGetInstanceProcAddr: {}", SDL_GetError());
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(loader);
}

VkSurfaceKHR Window::createSurface(VkInstance instance) const {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(m_window, instance, nullptr, &surface)) {
        FUMAR_FATAL("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError());
        FUMAR_DEBUG_BREAK();
        return VK_NULL_HANDLE;
    }
    FUMAR_DEBUG("vulkan surface created");
    return surface;
}

void Window::destroySurface(VkInstance instance, VkSurfaceKHR surface) const {
    if (surface != VK_NULL_HANDLE) {
        SDL_Vulkan_DestroySurface(instance, surface, nullptr);
    }
}

} // namespace fumar
