#pragma once

#include "fumar/scene/scene.hpp"
#include "fumar/script/script_context.hpp"

struct lua_State;

namespace fumar::script {

/// Installs the fumar table and the Node metatable into a fresh state.
void registerBindings(lua_State* lua);

/// Points the bindings at the scene they should operate on for this call.
///
/// Stored in the Lua registry rather than captured per binding, because the
/// scene changes between frames while the bindings are installed once. Set to
/// null outside an update, so a stray coroutine cannot touch a stale pointer.
void setActiveScene(lua_State* lua, Scene* scene);

Scene* activeScene(lua_State* lua);

/// Points the bindings at the input, camera and raycast for this frame.
///
/// Same reasoning as setActiveScene: installed once, changed every frame. Set
/// to null outside an update so nothing can reach a stale pointer.
void setActiveContext(lua_State* lua, ScriptContext* context);

/// Pushes a node handle onto the stack as Lua userdata.
void pushNode(lua_State* lua, NodeId id);

} // namespace fumar::script
