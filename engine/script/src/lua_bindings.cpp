#include "lua_bindings.hpp"

#include "fumar/core/log.hpp"
#include "fumar/core/math.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cmath>
#include <string>

namespace fumar::script {
namespace {

constexpr const char* kNodeMetatable = "fumar.Node";
constexpr const char* kSceneRegistryKey = "fumar.activeScene";

/// The node id carried by a Lua node handle.
///
/// An id rather than a Node* because the scene reallocates its storage when
/// nodes are added, and a script holding a pointer across that would be reading
/// freed memory. An id stays valid, and turns back into a node through a check
/// that also catches deletion.
struct NodeHandle {
    NodeId id;
};

Scene* sceneOrNull(lua_State* lua) {
    lua_getfield(lua, LUA_REGISTRYINDEX, kSceneRegistryKey);
    auto* scene = static_cast<Scene*>(lua_touserdata(lua, -1));
    lua_pop(lua, 1);
    return scene;
}

/// Resolves a node argument, raising a Lua error if it is gone.
///
/// Raising rather than returning null on purpose: a script that keeps a handle
/// to a deleted object has a bug, and a clear error naming the line beats
/// silently doing nothing.
Node& checkNode(lua_State* lua, int index) {
    auto* handle = static_cast<NodeHandle*>(luaL_checkudata(lua, index, kNodeMetatable));

    Scene* scene = sceneOrNull(lua);
    if (scene == nullptr) {
        luaL_error(lua, "no scene is active");
    }
    if (!scene->isAlive(handle->id)) {
        luaL_error(lua, "node %d no longer exists", static_cast<int>(handle->id));
    }
    return scene->node(handle->id);
}

// --- Node methods -----------------------------------------------------------

int nodeId(lua_State* lua) {
    auto* handle = static_cast<NodeHandle*>(luaL_checkudata(lua, 1, kNodeMetatable));
    lua_pushinteger(lua, static_cast<lua_Integer>(handle->id));
    return 1;
}

int nodeName(lua_State* lua) {
    const Node& node = checkNode(lua, 1);
    lua_pushstring(lua, node.name.c_str());
    return 1;
}

int nodeSetName(lua_State* lua) {
    Node& node = checkNode(lua, 1);
    node.name = luaL_checkstring(lua, 2);
    return 0;
}

int nodePosition(lua_State* lua) {
    const Node& node = checkNode(lua, 1);
    lua_pushnumber(lua, static_cast<lua_Number>(node.transform.position.x));
    lua_pushnumber(lua, static_cast<lua_Number>(node.transform.position.y));
    lua_pushnumber(lua, static_cast<lua_Number>(node.transform.position.z));
    return 3;
}

int nodeSetPosition(lua_State* lua) {
    Node& node = checkNode(lua, 1);
    node.transform.position = Vec3{
        static_cast<f32>(luaL_checknumber(lua, 2)),
        static_cast<f32>(luaL_checknumber(lua, 3)),
        static_cast<f32>(luaL_checknumber(lua, 4)),
    };
    return 0;
}

int nodeScale(lua_State* lua) {
    const Node& node = checkNode(lua, 1);
    lua_pushnumber(lua, static_cast<lua_Number>(node.transform.scale.x));
    lua_pushnumber(lua, static_cast<lua_Number>(node.transform.scale.y));
    lua_pushnumber(lua, static_cast<lua_Number>(node.transform.scale.z));
    return 3;
}

int nodeSetScale(lua_State* lua) {
    Node& node = checkNode(lua, 1);
    node.transform.scale = Vec3{
        static_cast<f32>(luaL_checknumber(lua, 2)),
        static_cast<f32>(luaL_checknumber(lua, 3)),
        static_cast<f32>(luaL_checknumber(lua, 4)),
    };
    return 0;
}

/// Rotation is exposed as degrees around each axis, not as a quaternion.
///
/// Scripts are written by hand, and nobody types quaternion components. The
/// scene keeps the quaternion; this converts at the boundary, exactly as the
/// inspector does.
int nodeRotation(lua_State* lua) {
    const Node& node = checkNode(lua, 1);
    const Quat& q = node.transform.rotation;

    const f32 sinPitch = clamp(2.0f * (q.w * q.x - q.y * q.z), -1.0f, 1.0f);
    const f32 pitch = std::asin(sinPitch);
    const f32 yaw = std::atan2(2.0f * (q.w * q.y + q.z * q.x), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const f32 roll = std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.x * q.x + q.z * q.z));

    lua_pushnumber(lua, static_cast<lua_Number>(degrees(pitch)));
    lua_pushnumber(lua, static_cast<lua_Number>(degrees(yaw)));
    lua_pushnumber(lua, static_cast<lua_Number>(degrees(roll)));
    return 3;
}

int nodeSetRotation(lua_State* lua) {
    Node& node = checkNode(lua, 1);

    const f32 pitch = static_cast<f32>(luaL_checknumber(lua, 2));
    const f32 yaw = static_cast<f32>(luaL_checknumber(lua, 3));
    const f32 roll = static_cast<f32>(luaL_checknumber(lua, 4));

    node.transform.rotation = normalize(fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, radians(yaw)) *
                                        fromAxisAngle(Vec3{1.0f, 0.0f, 0.0f}, radians(pitch)) *
                                        fromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, radians(roll)));
    return 0;
}

int nodeVisible(lua_State* lua) {
    lua_pushboolean(lua, checkNode(lua, 1).visible ? 1 : 0);
    return 1;
}

int nodeSetVisible(lua_State* lua) {
    checkNode(lua, 1).visible = lua_toboolean(lua, 2) != 0;
    return 0;
}

int nodeChildCount(lua_State* lua) {
    lua_pushinteger(lua, static_cast<lua_Integer>(checkNode(lua, 1).children.size()));
    return 1;
}

/// One-based indexing, because that is what Lua does everywhere else and a
/// script that has to remember which convention applies where is a bug waiting
/// to happen.
int nodeChild(lua_State* lua) {
    const Node& node = checkNode(lua, 1);
    const auto index = static_cast<usize>(luaL_checkinteger(lua, 2));

    if (index < 1 || index > node.children.size()) {
        lua_pushnil(lua);
        return 1;
    }
    pushNode(lua, node.children[index - 1]);
    return 1;
}

int nodeParent(lua_State* lua) {
    const Node& node = checkNode(lua, 1);
    if (node.parent == kInvalidNode) {
        lua_pushnil(lua);
        return 1;
    }
    pushNode(lua, node.parent);
    return 1;
}

int nodeToString(lua_State* lua) {
    auto* handle = static_cast<NodeHandle*>(luaL_checkudata(lua, 1, kNodeMetatable));
    Scene* scene = sceneOrNull(lua);

    if (scene != nullptr && scene->isAlive(handle->id)) {
        lua_pushfstring(lua, "node<%s #%d>", scene->node(handle->id).name.c_str(),
                        static_cast<int>(handle->id));
    } else {
        lua_pushfstring(lua, "node<dead #%d>", static_cast<int>(handle->id));
    }
    return 1;
}

const luaL_Reg kNodeMethods[] = {
    {"id", nodeId},
    {"name", nodeName},
    {"set_name", nodeSetName},
    {"position", nodePosition},
    {"set_position", nodeSetPosition},
    {"rotation", nodeRotation},
    {"set_rotation", nodeSetRotation},
    {"scale", nodeScale},
    {"set_scale", nodeSetScale},
    {"visible", nodeVisible},
    {"set_visible", nodeSetVisible},
    {"child_count", nodeChildCount},
    {"child", nodeChild},
    {"parent", nodeParent},
    {nullptr, nullptr},
};

// --- the fumar table --------------------------------------------------------

int fumarLog(lua_State* lua) {
    FUMAR_INFO("[lua] {}", luaL_checkstring(lua, 1));
    return 0;
}

int fumarWarn(lua_State* lua) {
    FUMAR_WARN("[lua] {}", luaL_checkstring(lua, 1));
    return 0;
}

/// Finds the first node with this name. Linear, which is fine at editor scene
/// sizes and would want an index if it ever is not.
int fumarFind(lua_State* lua) {
    const char* name = luaL_checkstring(lua, 1);
    Scene* scene = sceneOrNull(lua);
    if (scene == nullptr) {
        lua_pushnil(lua);
        return 1;
    }

    NodeId found = kInvalidNode;
    scene->traverse([&](NodeId id, u32) {
        if (found == kInvalidNode && scene->node(id).name == name) {
            found = id;
        }
    });

    if (found == kInvalidNode) {
        lua_pushnil(lua);
    } else {
        pushNode(lua, found);
    }
    return 1;
}

const luaL_Reg kFumarFunctions[] = {
    {"log", fumarLog},
    {"warn", fumarWarn},
    {"find", fumarFind},
    {nullptr, nullptr},
};

} // namespace

void registerBindings(lua_State* lua) {
    // The metatable is its own __index, which is the standard way to give
    // userdata methods: node:position() looks up "position" in the metatable.
    luaL_newmetatable(lua, kNodeMetatable);

    lua_pushvalue(lua, -1);
    lua_setfield(lua, -2, "__index");

    lua_pushcfunction(lua, nodeToString);
    lua_setfield(lua, -2, "__tostring");

    // luaL_register with a null name registers into the table on the stack,
    // which is the Lua 5.1 way of doing what 5.2 calls luaL_setfuncs.
    luaL_register(lua, nullptr, kNodeMethods);
    lua_pop(lua, 1);

    luaL_register(lua, "fumar", kFumarFunctions);
    lua_pop(lua, 1);
}

void setActiveScene(lua_State* lua, Scene* scene) {
    lua_pushlightuserdata(lua, scene);
    lua_setfield(lua, LUA_REGISTRYINDEX, kSceneRegistryKey);
}

Scene* activeScene(lua_State* lua) {
    return sceneOrNull(lua);
}

void pushNode(lua_State* lua, NodeId id) {
    auto* handle = static_cast<NodeHandle*>(lua_newuserdata(lua, sizeof(NodeHandle)));
    handle->id = id;

    luaL_getmetatable(lua, kNodeMetatable);
    lua_setmetatable(lua, -2);
}

} // namespace fumar::script
