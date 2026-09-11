#pragma once

#include "fumar/native/component.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fumar {

/// Loads the game library, and runs the components in it.
///
/// The counterpart to ScriptEngine, for the other half of the dual scripting
/// model: a node names a Lua script OR a C++ component, the engine calls
/// on_start and on_update either way, and gameplay does not care which side of
/// the line it lives on.
///
/// What differs is the cost of a change. Lua recompiles in milliseconds, so its
/// Compile button is free. C++ has to be rebuilt by an actual compiler and the
/// library swapped underneath a running editor, which takes seconds and a good
/// deal more machinery - all of it here.
class NativeEngine {
public:
    /// `libraryDirectory` is where the built game library is looked for -
    /// beside the executable. Missing is not an error: a project with no C++
    /// gameplay yet simply has no components.
    explicit NativeEngine(std::filesystem::path libraryDirectory);
    ~NativeEngine();

    NativeEngine(const NativeEngine&) = delete;
    NativeEngine& operator=(const NativeEngine&) = delete;

    /// Drops every component instance, unloads the library, and loads it again.
    ///
    /// Returns how many component types the reloaded library registered. Zero
    /// with a message in errors() means it did not load.
    u32 reload(Scene& scene);

    /// Rebuilds the library by invoking the build system, then reloads it.
    ///
    /// Only possible in a development tree - a distributed build has neither a
    /// build directory nor a compiler, and says so rather than pretending. This
    /// is the part that takes seconds rather than milliseconds, and the reason
    /// it blocks: a half-rebuilt library must not be loaded.
    bool rebuild(Scene& scene);

    /// True when there is a build directory to rebuild in.
    bool canRebuild() const;

    /// Runs onStart for nodes that have not had it yet, then onUpdate for every
    /// node with a component.
    void update(Scene& scene, ScriptContext& context, f32 deltaSeconds);

    /// Forgets which nodes have started, so onStart runs again next update.
    void restart();

    /// Names the loaded library registered, for the editor to offer.
    const std::vector<std::string>& componentNames() const;

    /// Whatever went wrong loading or building, for the editor to show.
    const std::vector<std::string>& errors() const;

    bool has(const std::string& name) const;

    /// Where the source lives, so the editor can point at it.
    std::filesystem::path sourceDirectory() const;

    bool loaded() const;

private:
    // Pimpl: the OS library handle and the instance table stay out of this
    // header, so nothing that merely holds a NativeEngine compiles against
    // windows.h or dlfcn.h.
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace fumar
