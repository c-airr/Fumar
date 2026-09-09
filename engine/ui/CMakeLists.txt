# ---------------------------------------------------------------------------
#  fumar_ui - Dear ImGui wired into fumar's window and renderer.
#
#  Separate from fumar_render on purpose: the renderer accepts a callback that
#  records extra draw commands and knows nothing about panels, widgets or
#  input focus. A game that wants no interface links render and skips this.
# ---------------------------------------------------------------------------

add_library(fumar_ui STATIC
    src/imgui_layer.cpp)

add_library(fumar::ui ALIAS fumar_ui)

target_include_directories(fumar_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(fumar_ui
    PUBLIC
        fumar::core
        fumar::render
        # PUBLIC so applications can call ImGui:: directly to build their own
        # panels - that is the whole point of an immediate mode interface.
        fumar_imgui
    PRIVATE
        fumar::platform
        ${FUMAR_SDL_TARGET})

fumar_configure_target(fumar_ui)
