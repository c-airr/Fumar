#pragma once

#include "fumar/core/types.hpp"
#include "fumar/rhi/vk_common.hpp"

#include <string>
#include <vector>

namespace fumar::rhi {

struct InstanceDesc {
    /// The bootstrap entry point, obtained from the window (SDL already has the
    /// loader open, so borrowing its pointer avoids a second loader).
    PFN_vkGetInstanceProcAddr loader = nullptr;

    /// Extensions the windowing system needs, from Window::requiredVulkanExtensions().
    std::vector<const char*> requiredExtensions;

    std::string applicationName = "fumar";

    /// Validation layers check every call against the specification and report
    /// misuse. They cost performance, so they are on in debug builds only - but
    /// developing Vulkan without them means silently rendering nothing and not
    /// being told why.
    bool enableValidation = true;
};

/// The Vulkan instance: the connection between the application and the loader.
///
/// Also owns the debug messenger, because its lifetime has to be strictly
/// inside the instance's.
class Instance {
public:
    explicit Instance(const InstanceDesc& desc);
    ~Instance() = default;

    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    Instance(Instance&&) noexcept = default;
    Instance& operator=(Instance&&) noexcept = default;

    vk::Instance handle() const { return *m_instance; }

    bool validationEnabled() const { return m_validationEnabled; }

    /// The API version actually in use, which may be lower than what we asked
    /// for if the loader is older.
    u32 apiVersion() const { return m_apiVersion; }

private:
    // Declaration order is destruction order reversed: the messenger is
    // declared second, so it is destroyed FIRST, while the instance that
    // created it is still alive. Swapping these two lines produces a
    // use-after-free that only fires on shutdown.
    vk::UniqueInstance m_instance;
    vk::UniqueDebugUtilsMessengerEXT m_messenger;

    bool m_validationEnabled = false;
    u32 m_apiVersion = 0;
};

} // namespace fumar::rhi
