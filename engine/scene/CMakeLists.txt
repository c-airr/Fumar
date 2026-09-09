# ---------------------------------------------------------------------------
#  fumar_scene - the scene tree.
#
#  Depends on core and nothing else. No Vulkan, no SDL, no renderer: a scene is
#  data, and keeping it that way is what will let the editor load, edit and save
#  one without a GPU in the picture, and what will let it be unit-tested.
#
#  Meshes and materials are referred to by opaque handle, so the scene never
#  learns what a VkBuffer is.
# ---------------------------------------------------------------------------

add_library(fumar_scene STATIC
    src/scene.cpp)

add_library(fumar::scene ALIAS fumar_scene)

target_include_directories(fumar_scene PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(fumar_scene PUBLIC fumar::core)

fumar_configure_target(fumar_scene)
