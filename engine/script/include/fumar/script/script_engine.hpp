#pragma once

#include "fumar/core/types.hpp"
#include "fumar/scene/scene.hpp"
#include "fumar/script/script_context.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fumar {

/// A compile or runtime failure in one script, kept for the editor to show.
///
/// Scripts are written while the engine runs, so a broken one must never take
/// the editor down with it: the error is recorded, that script stops running,
/// and everything else carries on.
struct ScriptError {
    std::string script;
    std::string message;
};

/// Runs Lua scripts attached to scene nodes.
///
/// The model is the one every engine converges on: a script file declares
/// `on_start(node)` and `on_update(node, dt)`, a node names the script it
/// wants, and the engine calls those functions. Nothing in the script knows it
/// is being driven from C++, and nothing in C++ knows what a particular script
/// does.
///
/// LuaJIT rather than plain Lua because its JIT closes most of the gap to
/// native code, which is what makes it reasonable to run gameplay in it at all.
class ScriptEngine {
public:
    /// `scriptDirectory` is scanned for .lua files. Missing is not an error -
    /// a project simply has no scripts yet.
    explicit ScriptEngine(std::filesystem::path scriptDirectory);
    ~ScriptEngine();

    ScriptEngine(const ScriptEngine&) = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;

    /// Recompiles every script in the directory. This is the Compile button.
    ///
    /// Cheap enough to run on a keystroke: Lua compiles to bytecode in
    /// milliseconds, so there is no build step to wait through. That is the
    /// whole reason gameplay goes in scripts rather than in C++.
    ///
    /// Returns how many compiled cleanly; the rest are in errors().
    u32 compileAll();

    /// Runs on_start for nodes that have not had it yet, then on_update for
    /// every node with a script.
    ///
    /// The context carries everything outside the scene a script can reach -
    /// input, the camera, a way to trace a ray. It is passed by reference and
    /// read back afterwards, because a script may write to the camera and the
    /// caller needs to know whether it did.
    void update(Scene& scene, ScriptContext& context, f32 deltaSeconds);

    /// Forgets which nodes have already started, so on_start runs again on the
    /// next update. Used when entering play mode or after a recompile.
    void restart();

    /// Names of the scripts currently loaded, for the editor to offer.
    const std::vector<std::string>& scriptNames() const;

    const std::vector<ScriptError>& errors() const;

    bool has(const std::string& name) const;

    const std::filesystem::path& directory() const;

private:
    // Pimpl: lua_State and the whole Lua API stay out of this header, so
    // nothing that merely holds a ScriptEngine has to compile against LuaJIT.
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace fumar
