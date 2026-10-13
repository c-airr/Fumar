# ---------------------------------------------------------------------------
#  material_tests - is a material read, packed and framed the way it was meant?
#
#  The parts of the material system that are decisions about data rather than
#  calls into Vulkan: the tangent frame a normal map is read against, which
#  file of a texture set is which map, what a .fmat file says, how separate
#  roughness and metalness images are packed into one. Each of them fails
#  quietly - a normal map lit from the wrong side, a roughness map read as the
#  colour - so each is pinned here rather than noticed in a screenshot.
#
#  It links the renderer for those functions but never creates a device, so it
#  runs without a GPU, CI included:
#     build/<preset>/bin/material_tests
#
#  Exits 0 when every check passes and 1 otherwise.
# ---------------------------------------------------------------------------

add_executable(material_tests src/main.cpp)

target_link_libraries(material_tests PRIVATE
    fumar::core
    fumar::geometry
    fumar::render)

fumar_configure_target(material_tests)
