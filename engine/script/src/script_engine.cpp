#include "fumar/script/script_engine.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/core/log.hpp"
#include "lua_bindings.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <luajit.h>
#include <lualib.h>
}

#include <algorithm>
#include <unordered_set>

namespace fumar {
namespace {

constexpr const char* kScriptTableKey = "fumar.scripts";

/// Turns the error object on top of the stack into a string and pops it.
std::string takeError(lua_State* lua) {
    const char* text = lua_tostring(lua, -1);
    std::string message = text != nullptr ? text : "unknown error";
    lua_pop(lua, 1);
    return message;
}

} // namespace

struct ScriptEngine::State {
    lua_State* lua = nullptr;
    std::filesystem::path directory;

    std::vector<std::string> names;
    std::vector<ScriptError> errors;

    /// Nodes that have already had on_start called. Cleared by restart().
    std::unordered_set<NodeId> started;

    /// Pushes the environment table of a named script, or nothing if it is not
    /// loaded. Returns whether it pushed.
    bool pushScriptEnvironment(const std::string& name) {
        lua_getfield(lua, LUA_REGISTRYINDEX, kScriptTableKey);
        lua_getfield(lua, -1, name.c_str());

        if (lua_isnil(lua, -1)) {
            lua_pop(lua, 2);
            return false;
        }
        // Drop the container table, leaving just the environment.
        lua_remove(lua, -2);
        return true;
    }

    /// Calls a function from a script environment with (node, extra...).
    ///
    /// Errors are logged once and the script is left in place: a script that
    /// throws every frame would otherwise flood the log, and one that throws
    /// once is usually recoverable.
    void callHandler(const std::string& script, const char* function, NodeId node, f32 deltaSeconds,
                     bool passDelta) {
        if (!pushScriptEnvironment(script)) {
            return;
        }

        lua_getfield(lua, -1, function);
        if (!lua_isfunction(lua, -1)) {
            // Not every script defines every handler, which is normal.
            lua_pop(lua, 2);
            return;
        }

        script::pushNode(lua, node);
        int argumentCount = 1;
        if (passDelta) {
            lua_pushnumber(lua, static_cast<lua_Number>(deltaSeconds));
            ++argumentCount;
        }

        if (lua_pcall(lua, argumentCount, 0, 0) != 0) {
            const std::string message = takeError(lua);
            FUMAR_ERROR("script '{}' failed in {}: {}", script, function, message);
            errors.push_back(ScriptError{script, message});
        }

        lua_pop(lua, 1); // environment
    }
};

ScriptEngine::ScriptEngine(std::filesystem::path scriptDirectory)
    : m_state(std::make_unique<State>()) {
    m_state->directory = std::move(scriptDirectory);

    m_state->lua = luaL_newstate();
    FUMAR_VERIFY_MSG(m_state->lua != nullptr, "could not create a Lua state");

    // The standard library, minus nothing for now. A shipping game would drop
    // io and os so a script cannot touch the filesystem, but during development
    // being able to print and read a file is worth more than the sandbox.
    luaL_openlibs(m_state->lua);

    script::registerBindings(m_state->lua);

    // The table holding one environment per script lives in the registry, where
    // scripts cannot reach it and overwrite each other.
    lua_newtable(m_state->lua);
    lua_setfield(m_state->lua, LUA_REGISTRYINDEX, kScriptTableKey);

    FUMAR_INFO("lua ready ({}), scripts from '{}'", LUAJIT_VERSION, m_state->directory.string());
}

ScriptEngine::~ScriptEngine() {
    if (m_state->lua != nullptr) {
        lua_close(m_state->lua);
    }
}

u32 ScriptEngine::compileAll() {
    lua_State* lua = m_state->lua;

    m_state->names.clear();
    m_state->errors.clear();

    // A fresh table drops every previously loaded script, so a deleted file
    // stops running and a renamed function does not linger from the old copy.
    lua_newtable(lua);
    lua_setfield(lua, LUA_REGISTRYINDEX, kScriptTableKey);

    std::error_code ec;
    if (!std::filesystem::exists(m_state->directory, ec)) {
        FUMAR_INFO("no scripts directory at '{}'", m_state->directory.string());
        return 0;
    }

    u32 compiled = 0;
    for (const auto& entry : std::filesystem::directory_iterator(m_state->directory, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".lua") {
            continue;
        }

        const std::string name = entry.path().stem().string();
        const std::string path = entry.path().string();

        // Compiles to bytecode without running anything, so a syntax error is
        // caught here rather than halfway through the first frame.
        if (luaL_loadfile(lua, path.c_str()) != 0) {
            const std::string message = takeError(lua);
            FUMAR_ERROR("script '{}' failed to compile: {}", name, message);
            m_state->errors.push_back(ScriptError{name, message});
            continue;
        }

        // Each script gets its own environment table, so two scripts can both
        // define on_update without colliding. Its metatable falls back to the
        // globals, so the standard library and the fumar table stay reachable.
        lua_newtable(lua);

        lua_newtable(lua);
        lua_getglobal(lua, "_G");
        lua_setfield(lua, -2, "__index");
        lua_setmetatable(lua, -2);

        // setfenv is the Lua 5.1 way to sandbox a chunk. 5.2 replaced it with
        // _ENV upvalues, but LuaJIT is 5.1 and this is the API it has.
        lua_pushvalue(lua, -1);
        lua_setfenv(lua, -3);

        // Stack: chunk, environment. Run the chunk so its function definitions
        // land in that environment.
        lua_insert(lua, -2);
        if (lua_pcall(lua, 0, 0, 0) != 0) {
            const std::string message = takeError(lua);
            FUMAR_ERROR("script '{}' failed to run: {}", name, message);
            m_state->errors.push_back(ScriptError{name, message});
            lua_pop(lua, 1); // environment
            continue;
        }

        lua_getfield(lua, LUA_REGISTRYINDEX, kScriptTableKey);
        lua_pushvalue(lua, -2);
        lua_setfield(lua, -2, name.c_str());
        lua_pop(lua, 2); // script table, environment

        m_state->names.push_back(name);
        ++compiled;
    }

    std::sort(m_state->names.begin(), m_state->names.end());

    // Everything is freshly compiled, so anything that had started is running
    // against code that no longer exists.
    restart();

    FUMAR_INFO("compiled {} script(s), {} error(s)", compiled, m_state->errors.size());
    return compiled;
}

void ScriptEngine::update(Scene& scene, f32 deltaSeconds) {
    if (m_state->names.empty()) {
        return;
    }

    script::setActiveScene(m_state->lua, &scene);

    // Collected first rather than walked directly, because a script is free to
    // create or delete nodes and that would invalidate an iteration in progress.
    std::vector<NodeId> scripted;
    scene.traverse([&](NodeId id, u32) {
        if (!scene.node(id).script.empty()) {
            scripted.push_back(id);
        }
    });

    for (NodeId id : scripted) {
        if (!scene.isAlive(id)) {
            continue;
        }

        const std::string script = scene.node(id).script;
        if (!has(script)) {
            continue;
        }

        if (m_state->started.insert(id).second) {
            m_state->callHandler(script, "on_start", id, 0.0f, false);
        }
        if (scene.isAlive(id)) {
            m_state->callHandler(script, "on_update", id, deltaSeconds, true);
        }
    }

    // Cleared so nothing can reach a scene pointer outside an update.
    script::setActiveScene(m_state->lua, nullptr);
}

void ScriptEngine::restart() {
    m_state->started.clear();
}

const std::vector<std::string>& ScriptEngine::scriptNames() const {
    return m_state->names;
}

const std::vector<ScriptError>& ScriptEngine::errors() const {
    return m_state->errors;
}

bool ScriptEngine::has(const std::string& name) const {
    return std::find(m_state->names.begin(), m_state->names.end(), name) != m_state->names.end();
}

const std::filesystem::path& ScriptEngine::directory() const {
    return m_state->directory;
}

} // namespace fumar
