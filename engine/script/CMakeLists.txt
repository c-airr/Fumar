# ---------------------------------------------------------------------------
#  fumar_script - LuaJIT scripting.
#
#  Depends on scene and core, not on render: a script moves objects, renames
#  them and reads their transforms, none of which requires knowing what a
#  VkBuffer is. Keeping it that way means scripts can be tested without a GPU.
# ---------------------------------------------------------------------------

# The declarations, with none of the implementation behind them.
#
# ScriptContext - input, the camera, a raycast callback - is what a script can
# reach beyond the scene, and C++ components get exactly the same thing. But a
# component has no business linking LuaJIT, and linking it anyway is not just
# untidy: LuaJIT is an ExternalProject, so its build step re-runs on every
# build of anything that depends on it, and the editor's Compile button would
# spend most of its seconds waiting for a Lua interpreter nobody asked to
# rebuild.
#
# INTERFACE: carries include paths, compiles nothing.
add_library(fumar_script_api INTERFACE)
add_library(fumar::script_api ALIAS fumar_script_api)

target_include_directories(fumar_script_api INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(fumar_script_api INTERFACE fumar::core fumar::scene)

add_library(fumar_script STATIC
    src/lua_bindings.cpp
    src/script_context.cpp
    src/script_engine.cpp)

add_library(fumar::script ALIAS fumar_script)

target_link_libraries(fumar_script
    PUBLIC
        fumar::script_api
    # PRIVATE: lua_State never appears in our headers, so nothing above this
    # module needs the LuaJIT headers or has to know Lua is what runs scripts.
    PRIVATE
        fumar_luajit)

fumar_configure_target(fumar_script)
