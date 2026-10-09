# ---------------------------------------------------------------------------
#  geometry_tests - do the modelling operations leave a sound mesh behind?
#
#  Every operation in fumar_geometry rewrites a face list, and the ways that can
#  go wrong are quiet ones: a face wound backwards, an edge shared by three
#  faces, a vertex nothing uses. None of them crash. They render, more or less,
#  and then the NEXT operation walks across the broken spot and produces
#  garbage. So each test here performs an operation and then checks the result
#  for exactly those problems, plus the counts of vertices, edges and faces it
#  should have produced.
#
#  No window, no GPU - it links only core and geometry, so it runs anywhere,
#  CI included:
#     build/<preset>/bin/geometry_tests
#
#  Exit code is the number of failed checks.
# ---------------------------------------------------------------------------

add_executable(geometry_tests src/main.cpp)

target_link_libraries(geometry_tests PRIVATE
    fumar::core
    fumar::geometry)

fumar_configure_target(geometry_tests)
