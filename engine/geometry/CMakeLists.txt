# ---------------------------------------------------------------------------
#  fumar_geometry - polygon meshes that can be edited.
#
#  Depends on core and nothing else. No Vulkan, no renderer: an editable mesh
#  is positions and faces, and every modelling operation is arithmetic on
#  those. Keeping the GPU out of it is what lets tools/geometry_tests check
#  every operation in a plain process, with no window and no device - which
#  matters, because a broken topology does not crash. It renders, mostly, and
#  breaks the next operation instead.
#
#  The renderer depends on this, not the other way round: it turns a mesh into
#  triangles and vertex buffers. This module never learns what a buffer is.
# ---------------------------------------------------------------------------

add_library(fumar_geometry STATIC
    src/editable_mesh.cpp
    src/operations.cpp
    src/primitives.cpp)

add_library(fumar::geometry ALIAS fumar_geometry)

target_include_directories(fumar_geometry PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(fumar_geometry PUBLIC fumar::core)

fumar_configure_target(fumar_geometry)
