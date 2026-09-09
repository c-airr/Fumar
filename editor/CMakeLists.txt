# ---------------------------------------------------------------------------
#  fumar_editor - the editor application.
#
#  This is the executable you launch to get a viewport, a scene tree and an
#  inspector. It is an application built on the engine, not the engine itself:
#  the same libraries are linked by sandbox, and would be linked by a game.
# ---------------------------------------------------------------------------

add_executable(fumar_editor
    src/main.cpp
    src/panels.cpp)

target_link_libraries(fumar_editor PRIVATE
    fumar::core
    fumar::platform
    fumar::render
    fumar::scene
    fumar::ui)

fumar_configure_target(fumar_editor)

# The shaders are built by the fumar_shaders target; this only orders them
# before the executable so a fresh build never runs without them.
add_dependencies(fumar_editor fumar_shaders)

# Same asset mirroring as the sandbox: both executables live in the same
# directory, so this is idempotent, but stating it per target keeps each one
# self-contained.
if(EXISTS "${CMAKE_SOURCE_DIR}/assets")
    add_custom_command(TARGET fumar_editor POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
                "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:fumar_editor>/assets"
        COMMENT "copying assets")
endif()
