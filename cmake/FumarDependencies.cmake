# ============================================================================
#  External dependencies.
#
#  We use FetchContent: CMake clones the repositories into build/_deps/ during
#  the first configure and builds them alongside the engine. No submodules, no
#  package manager - everything is visible right here.
#
#  SYSTEM           - dependency headers are treated as system headers, so our
#                     own warnings (-Wall -Wextra) do not drown in their code.
#  EXCLUDE_FROM_ALL - only build what we actually use.
# ============================================================================

include(FetchContent)

set(FETCHCONTENT_QUIET OFF)  # show clone progress; the first run takes a while

# ---------------------------------------------------------------------------
# SDL3 - window, input, events.
# Built statically so the sandbox ends up as a single self-contained exe.
# ---------------------------------------------------------------------------
set(SDL_SHARED        OFF CACHE BOOL "" FORCE)
set(SDL_STATIC        ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY  OFF CACHE BOOL "" FORCE)
set(SDL_TESTS         OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES      OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL       OFF CACHE BOOL "" FORCE)

FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.16
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    SYSTEM
    EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# VulkanMemoryAllocator - GPU memory allocator.
# A header-only library: all of its code lives in vk_mem_alloc.h and only
# reaches the binary through engine/rhi/src/vma_impl.cpp.
# ---------------------------------------------------------------------------
FetchContent_Declare(VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    SYSTEM
    EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# cgltf - glTF 2.0 parser, and stb_image - image decoder.
#
# Both are single-header C libraries with no build system of their own. Naming
# a SOURCE_SUBDIR that does not exist tells FetchContent to download them and
# stop there, instead of looking for a CMakeLists.txt to add_subdirectory.
# ---------------------------------------------------------------------------
FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG        v1.15
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  no-cmake-here)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    # stb publishes no releases, so a commit is the only reproducible pin.
    GIT_TAG        2c980bb59875b0d32144a71867fbdebb2f77cd20
    SOURCE_SUBDIR  no-cmake-here)

# ---------------------------------------------------------------------------
# Dear ImGui - the editor's user interface, on the docking branch.
#
# Docking is not in the stable releases: it is what lets panels be dragged,
# split and torn off into separate windows, which is the whole shape of an
# editor. The -docking tags are official releases of that branch.
# ---------------------------------------------------------------------------
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b-docking
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  no-cmake-here)

FetchContent_MakeAvailable(SDL3 VulkanMemoryAllocator cgltf stb imgui)

# Header-only dependencies get a hand-written INTERFACE target. SYSTEM keeps
# their warnings out of our build log.
add_library(fumar_cgltf INTERFACE)
target_include_directories(fumar_cgltf SYSTEM INTERFACE "${cgltf_SOURCE_DIR}")

add_library(fumar_stb INTERFACE)
target_include_directories(fumar_stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# Target names differ between library versions, so we resolve them once here
# and the rest of the project uses our own variables.
# ---------------------------------------------------------------------------
if(TARGET SDL3::SDL3-static)
    set(FUMAR_SDL_TARGET SDL3::SDL3-static)
elseif(TARGET SDL3::SDL3)
    set(FUMAR_SDL_TARGET SDL3::SDL3)
else()
    message(FATAL_ERROR "fumar: no SDL3 target found after FetchContent")
endif()

if(TARGET GPUOpen::VulkanMemoryAllocator)
    set(FUMAR_VMA_TARGET GPUOpen::VulkanMemoryAllocator)
elseif(TARGET VulkanMemoryAllocator)
    set(FUMAR_VMA_TARGET VulkanMemoryAllocator)
else()
    message(FATAL_ERROR "fumar: no VulkanMemoryAllocator target found")
endif()

# ---------------------------------------------------------------------------
# ImGui ships loose source files rather than a build system, so the library is
# assembled here. The core four files plus the two backends we use: SDL3 for
# input and window handling, Vulkan for drawing.
# ---------------------------------------------------------------------------
add_library(fumar_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")

target_include_directories(fumar_imgui SYSTEM PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends")

target_link_libraries(fumar_imgui
    PUBLIC Vulkan::Headers
    PRIVATE ${FUMAR_SDL_TARGET})

# fumar never links vulkan-1, so ImGui must not expect the global entry points
# either - it gets them from us through ImGui_ImplVulkan_LoadFunctions instead.
target_compile_definitions(fumar_imgui PUBLIC
    VK_NO_PROTOTYPES
    IMGUI_IMPL_VULKAN_NO_PROTOTYPES)

# Somebody else's code; our warning flags have nothing useful to say about it.
if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(fumar_imgui PRIVATE /w)
else()
    target_compile_options(fumar_imgui PRIVATE -w)
endif()

message(STATUS "fumar: SDL -> ${FUMAR_SDL_TARGET}, VMA -> ${FUMAR_VMA_TARGET}, ImGui -> fumar_imgui")
