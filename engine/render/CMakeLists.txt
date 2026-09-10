# ---------------------------------------------------------------------------
#  fumar_render - the layer that turns RHI objects into pictures.
#
#  The only module that depends on both fumar_rhi and fumar_platform: it is
#  where the window's surface meets the Vulkan device. Keeping that junction in
#  exactly one place is what lets the two sides below it stay unaware of each
#  other.
# ---------------------------------------------------------------------------

add_library(fumar_render STATIC
    src/camera.cpp
    src/gltf_loader.cpp
    src/mesh.cpp
    src/renderer.cpp
    src/resources.cpp
    src/scene_io.cpp
    src/texture.cpp
    src/third_party_impl.cpp)

add_library(fumar::render ALIAS fumar_render)

target_include_directories(fumar_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(fumar_render
    # All PUBLIC: renderer.hpp exposes vk:: types from the RHI, and camera.hpp
    # names Window in its interface, so consumers need both sets of headers.
    PUBLIC
        fumar::core
        fumar::platform
        fumar::rhi
        fumar::scene
    # PRIVATE: the glTF parser and image decoder are used only inside .cpp
    # files, so their headers stay out of everyone else's include path.
    PRIVATE
        fumar_cgltf
        fumar_stb
        nlohmann_json::nlohmann_json)

fumar_configure_target(fumar_render)

# third_party_impl.cpp compiles somebody else's code, which our warning flags
# have nothing useful to say about.
if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set_source_files_properties(src/third_party_impl.cpp PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(src/third_party_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
