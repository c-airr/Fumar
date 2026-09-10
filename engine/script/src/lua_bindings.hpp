#pragma once

#include "fumar/scene/scene.hpp"

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

/// Pushes a node handle onto the stack as Lua userdata.
void pushNode(lua_State* lua, NodeId id);

} // namespace fumar::script
