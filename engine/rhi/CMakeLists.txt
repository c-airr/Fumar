# ---------------------------------------------------------------------------
#  fumar_rhi - Render Hardware Interface: the Vulkan layer.
#
#  Note what is NOT here: fumar_platform. The RHI never talks to the window
#  system. It is handed a ready-made VkSurfaceKHR from above, which keeps it
#  testable without a window and stops the graphics backend from being welded
#  to SDL.
# ---------------------------------------------------------------------------

add_library(fumar_rhi STATIC
    src/buffer.cpp
    src/descriptor.cpp
    src/device.cpp
    src/frame_context.cpp
    src/image.cpp
    src/instance.cpp
    src/pipeline.cpp
    src/swapchain.cpp
    src/upload_context.cpp
    src/vma_impl.cpp)

add_library(fumar::rhi ALIAS fumar_rhi)

target_include_directories(fumar_rhi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(fumar_rhi
    # PUBLIC: our headers expose vk:: types, so consumers need the same Vulkan
    # headers configured with the same macros.
    PUBLIC
        fumar::core
        fumar::vulkan
    # PRIVATE: VMA only appears inside .cpp files.
    PRIVATE
        ${FUMAR_VMA_TARGET})

# VMA normally calls the global vkAllocateMemory and friends directly. With
# VK_NO_PROTOTYPES those do not exist, so it is switched to resolving them at
# runtime from the two bootstrap pointers Device hands it.
target_compile_definitions(fumar_rhi PRIVATE
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
    VMA_VULKAN_VERSION=1003000)

fumar_configure_target(fumar_rhi)

# vma_impl.cpp compiles ~20k lines of somebody else's code. Our warning flags
# have nothing useful to say about it, so they are switched off for this one
# file - the rest of the module keeps them.
if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set_source_files_properties(src/vma_impl.cpp PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(src/vma_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
