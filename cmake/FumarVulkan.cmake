# ============================================================================
#  Vulkan configuration, kept in exactly one place.
#
#  vulkan.hpp changes shape depending on which macros are defined before it is
#  included. If two translation units disagree, the class layouts they see
#  differ and the linker happily merges them anyway - an ODR violation that
#  shows up as a crash at runtime, not as a compile error. Defining the macros
#  on an INTERFACE target guarantees every consumer sees the same header.
#
#  VK_NO_PROTOTYPES
#      Stops the C headers from declaring the global vkCreateDevice() and
#      friends. We never link vulkan-1, so a call to one of those would fail at
#      link time; with this macro it fails at compile time instead, which is a
#      much clearer error. All calls go through the dynamic dispatcher.
#
#  VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
#      Every entry point is resolved at runtime through vkGetInstanceProcAddr.
#      This is what volk does for C projects; vulkan.hpp has it built in.
#
#  VULKAN_HPP_NO_CONSTRUCTORS
#      Removes the generated struct constructors so the structs stay aggregates
#      and C++20 designated initialisers work:
#          vk::ImageCreateInfo{.imageType = vk::ImageType::e2D, ...}
#      Far more readable than positional arguments, and unset fields keep their
#      correct defaults (sType in particular).
# ============================================================================

add_library(fumar_vulkan INTERFACE)
add_library(fumar::vulkan ALIAS fumar_vulkan)

target_link_libraries(fumar_vulkan INTERFACE Vulkan::Headers)

target_compile_definitions(fumar_vulkan INTERFACE
    VK_NO_PROTOTYPES
    VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
    VULKAN_HPP_NO_CONSTRUCTORS)
