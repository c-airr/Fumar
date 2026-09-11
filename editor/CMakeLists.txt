# ---------------------------------------------------------------------------
#  fumar_editor - the editor application.
#
#  This is the executable you launch to get a viewport, a scene tree and an
#  inspector. It is an application built on the engine, not the engine itself:
#  the same libraries are linked by sandbox, and would be linked by a game.
# ---------------------------------------------------------------------------

add_executable(fumar_editor
    src/history.cpp
    src/main.cpp
    src/panels.cpp)

target_link_libraries(fumar_editor PRIVATE
    fumar::core
    fumar::platform
    fumar::render
    fumar::scene
    fumar::native
    fumar::script
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

# Where Lua scripts are edited. Same idea as FUMAR_GAME_SOURCE_DIR for C++:
# a development build works on the SOURCE, not on the copy made beside the
# executable. Absent from a distributed build, where the copy is all there is.
target_compile_definitions(fumar_editor PRIVATE
    FUMAR_LUA_SOURCE_DIR="${CMAKE_SOURCE_DIR}/scripts/lua")

# The interface fonts, from ImGui's own misc/fonts directory.
if(EXISTS "${FUMAR_IMGUI_FONT_DIR}")
    add_custom_command(TARGET fumar_editor POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:fumar_editor>/fonts"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${FUMAR_IMGUI_FONT_DIR}/Roboto-Medium.ttf"
                "${FUMAR_IMGUI_FONT_DIR}/Cousine-Regular.ttf"
                "$<TARGET_FILE_DIR:fumar_editor>/fonts"
        COMMENT "copying interface fonts")
endif()

# Lua sources are mirrored next to the executable, where ScriptEngine looks for
# them. Copied rather than referenced in place so a built editor is
# self-contained - and copy_directory_if_different means editing a script and
# rebuilding picks it up without a full copy.
if(EXISTS "${CMAKE_SOURCE_DIR}/scripts/lua")
    add_custom_command(TARGET fumar_editor POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
                "${CMAKE_SOURCE_DIR}/scripts/lua"
                "$<TARGET_FILE_DIR:fumar_editor>/scripts/lua"
        COMMENT "copying lua scripts")
endif()
