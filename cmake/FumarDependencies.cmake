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

FetchContent_MakeAvailable(SDL3 VulkanMemoryAllocator cgltf stb)

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

message(STATUS "fumar: SDL -> ${FUMAR_SDL_TARGET}, VMA -> ${FUMAR_VMA_TARGET}")
