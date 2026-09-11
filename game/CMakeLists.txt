# ---------------------------------------------------------------------------
#  fumar_game - gameplay written in C++.
#
#  A SHARED library rather than part of the editor, and that is the whole
#  feature: a shared library can be replaced while the process that loaded it
#  is still running. Building this into the executable would mean restarting
#  the editor for every change, which is exactly what scripting exists to
#  avoid.
#
#  This is the file to add your own sources to.
# ---------------------------------------------------------------------------

add_library(fumar_game SHARED
    src/components.cpp)

target_link_libraries(fumar_game PRIVATE
    fumar::core
    fumar::scene
    fumar::native)

# Beside the executable, where NativeEngine looks for it. On Windows a DLL is a
# RUNTIME artifact and on Linux a .so is a LIBRARY one, so both have to be said.
set_target_properties(fumar_game PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"

    # No "lib" prefix difference to reason about at load time beyond the one
    # NativeEngine already handles, and hidden visibility so the only symbol
    # that escapes is the one marked for export.
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

fumar_configure_target(fumar_game)
