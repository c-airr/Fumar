# ---------------------------------------------------------------------------
#  fumar_platform - windowing, input and OS paths, backed by SDL3.
#
#  This is the only module that is allowed to include SDL headers. Everything
#  above it talks to fumar::Window instead, so replacing SDL with a hand-written
#  Win32/X11 backend later would touch this directory and nothing else.
# ---------------------------------------------------------------------------

add_library(fumar_platform STATIC
    src/paths.cpp
    src/window.cpp)

add_library(fumar::platform ALIAS fumar_platform)

target_include_directories(fumar_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(fumar_platform
    # PUBLIC: window.hpp includes <vulkan/vulkan_core.h> for the handle types,
    # so consumers need the Vulkan include path too. Vulkan::Headers carries
    # only headers - the vulkan-1 library is deliberately not linked.
    PUBLIC
        fumar::core
        Vulkan::Headers
    # PRIVATE: SDL is an implementation detail. Linking it PUBLIC would put SDL
    # headers on every consumer's include path and quietly undo the abstraction.
    PRIVATE
        ${FUMAR_SDL_TARGET})

fumar_configure_target(fumar_platform)
