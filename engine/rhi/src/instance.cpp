#include "fumar/rhi/instance.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/core/log.hpp"

#include <algorithm>
#include <string_view>

// Exactly one translation unit in the program has to provide storage for the
// dispatcher object that every VULKAN_HPP_DEFAULT_DISPATCHER expression refers
// to. This macro defines it. Putting it in a second file gives a duplicate
// symbol at link time; omitting it gives an undefined one.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace fumar::rhi {
namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

bool isLayerAvailable(std::string_view name) {
    const auto layers = vk::enumerateInstanceLayerProperties();
    return std::any_of(layers.begin(), layers.end(), [name](const vk::LayerProperties& layer) {
        return std::string_view(layer.layerName.data()) == name;
    });
}

bool isExtensionAvailable(std::string_view name) {
    const auto extensions = vk::enumerateInstanceExtensionProperties();
    return std::any_of(extensions.begin(), extensions.end(), [name](const vk::ExtensionProperties& ext) {
        return std::string_view(ext.extensionName.data()) == name;
    });
}

log::Level severityToLogLevel(vk::DebugUtilsMessageSeverityFlagBitsEXT severity) {
    using Severity = vk::DebugUtilsMessageSeverityFlagBitsEXT;
    switch (severity) {
    case Severity::eVerbose: return log::Level::Trace;
    case Severity::eInfo: return log::Level::Debug;
    case Severity::eWarning: return log::Level::Warn;
    case Severity::eError: return log::Level::Error;
    default: return log::Level::Info;
    }
}

/// The parameter types vulkan.hpp expects the debug callback to take.
///
/// This is the one place where the two vulkan.hpp generations we build against
/// disagree. The Windows SDK 1.4 header declares the callback pointer in terms
/// of its own wrappers (`vk::DebugUtilsMessageSeverityFlagBitsEXT`,
/// `vk::DebugUtilsMessageTypeFlagsEXT`); the header shipped by Ubuntu declares
/// it with the plain C types. Writing the function either way compiles on one
/// machine and fails on the other, and casting the function pointer to fit is
/// worse still: calling a function through an incompatible pointer type is
/// undefined behaviour, and clang says so.
///
/// So instead of naming the types, we ask the header what they are. The
/// specialisation takes the pointer type apart into its parameters - including
/// the calling convention baked into VKAPI_PTR, which is part of the type on
/// Windows - and the callback below is declared from the pieces. Whatever the
/// header wants, that is exactly what gets defined.
template <typename Pfn>
struct MessengerCallbackTraits;

template <typename Result, typename Severity, typename Types, typename Data, typename User>
struct MessengerCallbackTraits<Result (VKAPI_PTR*)(Severity, Types, Data, User)> {
    using SeverityArg = Severity;
    using TypesArg = Types;
    using DataArg = Data;
};

using MessengerTraits =
    MessengerCallbackTraits<decltype(vk::DebugUtilsMessengerCreateInfoEXT{}.pfnUserCallback)>;

/// Called by the validation layers for every message they produce.
///
/// VKAPI_ATTR/VKAPI_CALL pin the calling convention: this function is invoked
/// from the driver, so it has to match what the driver expects rather than
/// whatever the compiler would pick by default.
VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(MessengerTraits::SeverityArg rawSeverity,
                                             MessengerTraits::TypesArg rawTypes,
                                             MessengerTraits::DataArg data,
                                             void* /*userData*/) {
    // On the newer header these casts are the identity; on the older one they
    // wrap the raw values. vk::Flags is a uint32_t and the enum is its
    // underlying type, so either way this is a relabelling, not a conversion.
    const auto severity = static_cast<vk::DebugUtilsMessageSeverityFlagBitsEXT>(rawSeverity);
    const auto types = static_cast<vk::DebugUtilsMessageTypeFlagsEXT>(rawTypes);

    const auto level = severityToLogLevel(severity);
    if (log::enabled(level) && data != nullptr) {
        const auto typeString = vk::to_string(types);
        const char* id = data->pMessageIdName != nullptr ? data->pMessageIdName : "?";
        const char* message = data->pMessage != nullptr ? data->pMessage : "";

        // Empty file name: these messages come from the driver, so pointing at
        // a line in our source would be misleading.
        log::detail::write(level, {}, 0, std::format("[{}] {}: {}", typeString, id, message));
    }

    // VK_FALSE means "carry on". Returning VK_TRUE aborts the very call that
    // triggered the message, which is only useful when hunting a specific bug.
    return VK_FALSE;
}

vk::DebugUtilsMessengerCreateInfoEXT makeMessengerInfo() {
    using Severity = vk::DebugUtilsMessageSeverityFlagBitsEXT;
    using Type = vk::DebugUtilsMessageTypeFlagBitsEXT;

    return vk::DebugUtilsMessengerCreateInfoEXT{
        // Warnings and errors only. eInfo and eVerbose are dominated by the
        // loader narrating which manifests it found and which layers it
        // inserted - hundreds of lines per launch that say nothing about
        // whether our code is correct.
        .messageSeverity = Severity::eWarning | Severity::eError,
        .messageType = Type::eGeneral | Type::eValidation | Type::ePerformance,
        .pfnUserCallback = debugCallback,
    };
}

} // namespace

Instance::Instance(const InstanceDesc& desc) {
    FUMAR_VERIFY_MSG(desc.loader != nullptr, "InstanceDesc::loader must be set");

    // --- step 1: bootstrap the dispatcher ---------------------------------
    // At this point only the entry points that exist without an instance are
    // available: vkEnumerateInstanceVersion, vkCreateInstance and a couple of
    // enumeration calls.
    VULKAN_HPP_DEFAULT_DISPATCHER.init(desc.loader);

    m_apiVersion = vk::enumerateInstanceVersion();
    FUMAR_INFO("vulkan loader supports API {}.{}.{}", vk::apiVersionMajor(m_apiVersion),
               vk::apiVersionMinor(m_apiVersion), vk::apiVersionPatch(m_apiVersion));
    FUMAR_VERIFY_MSG(m_apiVersion >= vk::ApiVersion13,
                     "fumar needs Vulkan 1.3 or newer; the installed loader reports {}.{}",
                     vk::apiVersionMajor(m_apiVersion), vk::apiVersionMinor(m_apiVersion));

    // --- step 2: layers and extensions ------------------------------------
    m_validationEnabled = desc.enableValidation;
    if (m_validationEnabled && !isLayerAvailable(kValidationLayer)) {
        FUMAR_WARN("{} not found - install the Vulkan SDK for validation", kValidationLayer);
        m_validationEnabled = false;
    }
    if (m_validationEnabled && !isExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        FUMAR_WARN("{} not available, validation output would be silent", VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        m_validationEnabled = false;
    }

    std::vector<const char*> extensions = desc.requiredExtensions;
    std::vector<const char*> layers;
    if (m_validationEnabled) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back(kValidationLayer);
    }

    for (const char* extension : extensions) {
        FUMAR_VERIFY_MSG(isExtensionAvailable(extension), "instance extension '{}' is not available",
                         extension);
        FUMAR_DEBUG("instance extension: {}", extension);
    }

    // --- step 3: create the instance --------------------------------------
    const vk::ApplicationInfo appInfo{
        .pApplicationName = desc.applicationName.c_str(),
        .applicationVersion = vk::makeApiVersion(0u, 0u, 1u, 0u),
        .pEngineName = "fumar",
        .engineVersion = vk::makeApiVersion(0u, 0u, 1u, 0u),
        .apiVersion = vk::ApiVersion13,
    };

    // Chaining the messenger info into pNext makes the layers report problems
    // during vkCreateInstance and vkDestroyInstance too - the window in which
    // the standalone messenger below does not yet, or no longer, exist.
    const vk::DebugUtilsMessengerCreateInfoEXT messengerInfo = makeMessengerInfo();

    const vk::InstanceCreateInfo createInfo{
        .pNext = m_validationEnabled ? &messengerInfo : nullptr,
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = static_cast<u32>(layers.size()),
        .ppEnabledLayerNames = layers.data(),
        .enabledExtensionCount = static_cast<u32>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };

    m_instance = vk::createInstanceUnique(createInfo);

    // --- step 4: load the instance-level entry points ----------------------
    // Without this second init() every vkCreateDevice, vkGetPhysicalDevice* and
    // surface call would still be a null pointer.
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*m_instance);

    if (m_validationEnabled) {
        m_messenger = m_instance->createDebugUtilsMessengerEXTUnique(messengerInfo);
        FUMAR_INFO("vulkan instance created with validation enabled");
    } else {
        FUMAR_INFO("vulkan instance created without validation");
    }
}

} // namespace fumar::rhi
