# ---------------------------------------------------------------------------
#  fumar_core - the foundation layer.
#
#  Deliberately depends on nothing: no Vulkan, no SDL, no third-party library.
#  Everything else in the engine is allowed to depend on core, and core is
#  allowed to depend on nothing, which is what keeps the dependency graph a
#  graph and not a knot.
# ---------------------------------------------------------------------------

add_library(fumar_core STATIC
    src/assert.cpp
    src/file.cpp
    src/log.cpp)

add_library(fumar::core ALIAS fumar_core)

# PUBLIC, because anything that links fumar_core also includes its headers.
# PRIVATE here would compile core itself but leave every consumer unable to
# find <fumar/core/log.hpp> - the single most common CMake mistake.
target_include_directories(fumar_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

fumar_configure_target(fumar_core)
