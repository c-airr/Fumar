#include "fumar/rhi/device.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/core/log.hpp"
#include "fumar/rhi/instance.hpp"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <set>
#include <string_view>
#include <vector>

namespace fumar::rhi {
namespace {

const std::vector<const char*> kRequiredDeviceExtensions{
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

QueueFamilies findQueueFamilies(vk::PhysicalDevice device, vk::SurfaceKHR surface) {
    QueueFamilies families;

    const auto properties = device.getQueueFamilyProperties();
    for (u32 index = 0; index < static_cast<u32>(properties.size()); ++index) {
        if (properties[index].queueCount == 0) {
            continue;
        }

        if (families.graphics == kInvalidQueueFamily &&
            (properties[index].queueFlags & vk::QueueFlagBits::eGraphics)) {
            families.graphics = index;
        }

        // Presentation support is a property of the (device, family, surface)
        // triple, not of the family alone - which is why the surface has to
        // exist before the device is picked.
        if (families.present == kInvalidQueueFamily &&
            device.getSurfaceSupportKHR(index, surface) == VK_TRUE) {
            families.present = index;
        }

        if (families.complete()) {
            break;
        }
    }

    return families;
}

bool supportsRequiredExtensions(vk::PhysicalDevice device) {
    const auto available = device.enumerateDeviceExtensionProperties();

    for (const char* required : kRequiredDeviceExtensions) {
        const bool found = std::any_of(available.begin(), available.end(),
                                       [required](const vk::ExtensionProperties& extension) {
                                           return std::string_view(extension.extensionName.data()) ==
                                                  required;
                                       });
        if (!found) {
            return false;
        }
    }
    return true;
}

bool supportsRequiredFeatures(vk::PhysicalDevice device) {
    // getFeatures2 walks a pNext chain: the base struct carries the Vulkan 1.0
    // features, and each linked struct carries the ones added by a later
    // version. StructureChain wires the pointers up for us.
    const auto chain =
        device.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan13Features>();
    const auto& features13 = chain.get<vk::PhysicalDeviceVulkan13Features>();

    return features13.dynamicRendering == VK_TRUE && features13.synchronization2 == VK_TRUE;
}

/// Ranks a GPU. Higher is better; zero means unusable.
u32 scoreDevice(const vk::PhysicalDeviceProperties& properties) {
    u32 score = 0;
    switch (properties.deviceType) {
    case vk::PhysicalDeviceType::eDiscreteGpu: score += 10000; break;
    case vk::PhysicalDeviceType::eIntegratedGpu: score += 1000; break;
    case vk::PhysicalDeviceType::eVirtualGpu: score += 100; break;
    case vk::PhysicalDeviceType::eCpu: score += 10; break;
    default: break;
    }

    // A tie-breaker between GPUs of the same class: the maximum 2D image size
    // tracks overall capability closely enough for this purpose.
    score += properties.limits.maxImageDimension2D / 16;
    return score;
}

} // namespace

Device::Device(const Instance& instance, vk::SurfaceKHR surface) {
    // --- pick a physical device --------------------------------------------
    const auto candidates = instance.handle().enumeratePhysicalDevices();
    FUMAR_VERIFY_MSG(!candidates.empty(), "no Vulkan-capable GPU found");

    u32 bestScore = 0;
    for (const vk::PhysicalDevice& candidate : candidates) {
        const auto properties = candidate.getProperties();
        const std::string_view name(properties.deviceName.data());

        if (properties.apiVersion < vk::ApiVersion13) {
            FUMAR_DEBUG("skipping '{}': driver exposes API {}.{}, need 1.3", name,
                        vk::apiVersionMajor(properties.apiVersion),
                        vk::apiVersionMinor(properties.apiVersion));
            continue;
        }
        if (!supportsRequiredExtensions(candidate)) {
            FUMAR_DEBUG("skipping '{}': missing {}", name, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            continue;
        }
        if (!supportsRequiredFeatures(candidate)) {
            FUMAR_DEBUG("skipping '{}': no dynamicRendering or synchronization2", name);
            continue;
        }

        const QueueFamilies families = findQueueFamilies(candidate, surface);
        if (!families.complete()) {
            FUMAR_DEBUG("skipping '{}': no queue family can both render and present", name);
            continue;
        }

        // An empty format or present-mode list means this GPU cannot drive this
        // surface at all, whatever its queues claim.
        if (candidate.getSurfaceFormatsKHR(surface).empty() ||
            candidate.getSurfacePresentModesKHR(surface).empty()) {
            FUMAR_DEBUG("skipping '{}': surface reports no usable formats", name);
            continue;
        }

        const u32 score = scoreDevice(properties);
        FUMAR_DEBUG("candidate '{}' scores {}", name, score);

        if (score > bestScore) {
            bestScore = score;
            m_physicalDevice = candidate;
            m_properties = properties;
            m_queueFamilies = families;
        }
    }

    FUMAR_VERIFY_MSG(m_physicalDevice, "no GPU meets fumar's requirements (Vulkan 1.3, swapchain, "
                                       "dynamic rendering, synchronization2)");

    FUMAR_INFO("GPU: {} (driver {}.{}.{}, API {}.{}.{})", std::string_view(m_properties.deviceName.data()),
               vk::apiVersionMajor(m_properties.driverVersion), vk::apiVersionMinor(m_properties.driverVersion),
               vk::apiVersionPatch(m_properties.driverVersion), vk::apiVersionMajor(m_properties.apiVersion),
               vk::apiVersionMinor(m_properties.apiVersion), vk::apiVersionPatch(m_properties.apiVersion));
    FUMAR_INFO("queue families: graphics {}, present {}", m_queueFamilies.graphics, m_queueFamilies.present);

    // --- open the logical device -------------------------------------------
    // One queue per distinct family. Graphics and present are usually the same
    // family, and asking for the same index twice is invalid, so they are
    // deduplicated here.
    const float queuePriority = 1.0f;
    const std::set<u32> uniqueFamilies{m_queueFamilies.graphics, m_queueFamilies.present};

    std::vector<vk::DeviceQueueCreateInfo> queueInfos;
    queueInfos.reserve(uniqueFamilies.size());
    for (u32 family : uniqueFamilies) {
        queueInfos.push_back(vk::DeviceQueueCreateInfo{
            .queueFamilyIndex = family,
            .queueCount = 1,
            .pQueuePriorities = &queuePriority,
        });
    }

    // Features are opt-in: a capability the GPU supports still has to be
    // switched on here, or using it is undefined behaviour. Vulkan 1.3 folded
    // both of these in from extensions.
    vk::PhysicalDeviceVulkan13Features features13{
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };

    const vk::DeviceCreateInfo createInfo{
        .pNext = &features13,
        .queueCreateInfoCount = static_cast<u32>(queueInfos.size()),
        .pQueueCreateInfos = queueInfos.data(),
        .enabledExtensionCount = static_cast<u32>(kRequiredDeviceExtensions.size()),
        .ppEnabledExtensionNames = kRequiredDeviceExtensions.data(),
    };

    m_device = m_physicalDevice.createDeviceUnique(createInfo);

    // Third and final dispatcher init. Device-level pointers skip the loader's
    // dispatch trampoline, so every draw call from here on is a direct jump.
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*m_device);

    m_graphicsQueue = m_device->getQueue(m_queueFamilies.graphics, 0);
    m_presentQueue = m_device->getQueue(m_queueFamilies.present, 0);

    // --- memory allocator ---------------------------------------------------
    // VMA normally calls the global vkAllocateMemory and friends, which do not
    // exist here (VK_NO_PROTOTYPES). Handing it the two bootstrap pointers lets
    // it resolve everything else itself - that is what VMA_DYNAMIC_VULKAN_
    // FUNCTIONS=1 in the CMake target switches on.
    VmaVulkanFunctions vulkanFunctions{};
    vulkanFunctions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
    vulkanFunctions.vkGetDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.physicalDevice = m_physicalDevice;
    allocatorInfo.device = *m_device;
    allocatorInfo.instance = instance.handle();
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    allocatorInfo.pVulkanFunctions = &vulkanFunctions;

    const VkResult result = vmaCreateAllocator(&allocatorInfo, &m_allocator);
    FUMAR_VERIFY_MSG(result == VK_SUCCESS, "vmaCreateAllocator failed with {}",
                     vk::to_string(static_cast<vk::Result>(result)));

    FUMAR_INFO("logical device and memory allocator ready");
}

Device::~Device() {
    // Runs before the members are destroyed, so the VkDevice that owns this
    // allocator is still alive here. Destroying it after m_device would be a
    // use-after-free.
    if (m_allocator != nullptr) {
        vmaDestroyAllocator(m_allocator);
        m_allocator = nullptr;
    }
}

vk::Format Device::findSupportedFormat(std::span<const vk::Format> candidates, vk::ImageTiling tiling,
                                       vk::FormatFeatureFlags features) const {
    for (vk::Format format : candidates) {
        const vk::FormatProperties properties = m_physicalDevice.getFormatProperties(format);

        // Which of the two feature sets applies depends on the tiling: linear
        // and optimal images can support entirely different things.
        const vk::FormatFeatureFlags supported = tiling == vk::ImageTiling::eLinear
                                                     ? properties.linearTilingFeatures
                                                     : properties.optimalTilingFeatures;
        if ((supported & features) == features) {
            return format;
        }
    }
    return vk::Format::eUndefined;
}

void Device::waitIdle() const {
    if (m_device) {
        m_device->waitIdle();
    }
}

} // namespace fumar::rhi
