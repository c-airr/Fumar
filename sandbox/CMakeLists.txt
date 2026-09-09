# ---------------------------------------------------------------------------
#  sandbox - the test application driving the engine.
#
#  Deliberately thin: it owns the window, the renderer and the main loop, and
#  nothing else. If something here starts looking like engine code, it belongs
#  in engine/.
# ---------------------------------------------------------------------------

add_executable(sandbox src/main.cpp)

target_link_libraries(sandbox PRIVATE
    fumar::core
    fumar::platform
    fumar::render
    fumar::scene)

fumar_configure_target(sandbox)

# Compiles the GLSL to SPIR-V and drops the .spv files next to the executable,
# in bin/shaders/ - which is where Renderer looks for them.
# The shaders are built by the fumar_shaders target; this only orders them
# before the executable so a fresh build never runs without them.
add_dependencies(sandbox fumar_shaders)

# Assets are optional and not tracked in git, but when the directory exists it
# is mirrored next to the executable so the renderer can find it at runtime.
if(EXISTS "${CMAKE_SOURCE_DIR}/assets")
    add_custom_command(TARGET sandbox POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
                "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:sandbox>/assets"
        COMMENT "copying assets")
endif()
