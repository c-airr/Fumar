# ---------------------------------------------------------------------------
#  fumar_script - LuaJIT scripting.
#
#  Depends on scene and core, not on render: a script moves objects, renames
#  them and reads their transforms, none of which requires knowing what a
#  VkBuffer is. Keeping it that way means scripts can be tested without a GPU.
# ---------------------------------------------------------------------------

add_library(fumar_script STATIC
    src/lua_bindings.cpp
    src/script_context.cpp
    src/script_engine.cpp)

add_library(fumar::script ALIAS fumar_script)

target_include_directories(fumar_script PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(fumar_script
    PUBLIC
        fumar::core
        fumar::scene
    # PRIVATE: lua_State never appears in our headers, so nothing above this
    # module needs the LuaJIT headers or has to know Lua is what runs scripts.
    PRIVATE
        fumar_luajit)

fumar_configure_target(fumar_script)
