# ---------------------------------------------------------------------------
#  native_smoke - does C++ hot reloading still work?
#
#  Not a unit test of a function but a test of a MECHANISM, and the reason it
#  exists is that the mechanism is the most fragile thing in the engine: it
#  depends on a compiler being reachable, on a library being replaceable while
#  the process holding it runs, on destructors being called before the code
#  that implements them is unloaded. Every one of those breaks silently.
#
#  Run it by hand:
#     build/<preset>/bin/native_smoke <bin dir> <path to game/src/components.cpp>
#
#  It edits that source, rebuilds, checks the new code is what runs, and puts
#  the file back - so the working tree is unchanged when it finishes.
#
#  Deliberately not part of the install rules: it is a development tool, and it
#  would be actively dangerous shipped beside a release, where "rebuild the
#  game library" means nothing and the file it edits does not exist.
# ---------------------------------------------------------------------------

add_executable(native_smoke src/main.cpp)

target_link_libraries(native_smoke PRIVATE
    fumar::core
    fumar::scene
    fumar::native)

fumar_configure_target(native_smoke)
