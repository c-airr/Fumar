#include "fumar/native/native_engine.hpp"

#include "fumar/core/log.hpp"

#include "dynamic_library.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fumar {
namespace {

/// The library target's name, matching game/CMakeLists.txt.
constexpr const char* kLibraryStem = "fumar_game";

/// The copy that is actually loaded.
///
/// Windows will not let a rebuild overwrite a DLL that is currently loaded, so
/// the editor loads a COPY and leaves the original free to be replaced. Doing
/// the same on Linux, where it is not strictly necessary, keeps one code path
/// rather than two.
constexpr const char* kLoadedStem = "fumar_game.live";

/// Registry handed to the library. Everything it stores is stored HERE, on the
/// engine side of the heap - see the note in component.hpp about why that
/// matters with a static CRT.
class Registry final : public ComponentRegistry {
public:
    void add(const char* name, Factory factory) override {
        if (name == nullptr || factory == nullptr) {
            return;
        }
        // Constructed from the const char* on this side, so the string's
        // memory belongs to the executable and is freed by it.
        factories.emplace(std::string(name), factory);
    }

    std::unordered_map<std::string, Factory> factories;
};

using RegisterFunction = void (*)(ComponentRegistry&);

/// The command that rebuilds the game library, assembled here rather than baked
/// into a macro. See the note in engine/native/CMakeLists.txt: quoting a
/// command line inside a CMake string that becomes a C++ string literal means
/// satisfying two parsers at once, and getting it wrong produces a compiler
/// error that points nowhere near the cause.
std::string buildCommand() {
#if defined(FUMAR_GAME_DEV_SCRIPT)
    return std::format(
        R"(powershell -NoProfile -ExecutionPolicy Bypass -File "{}" cmake --build "{}" --target fumar_game)",
        FUMAR_GAME_DEV_SCRIPT, FUMAR_GAME_BUILD_DIR);
#elif defined(FUMAR_GAME_BUILD_DIR)
    return std::format(R"(cmake --build "{}" --target fumar_game)", FUMAR_GAME_BUILD_DIR);
#else
    return {};
#endif
}

} // namespace

struct NativeEngine::State {
    std::filesystem::path directory;

    native::DynamicLibrary library;
    Registry registry;

    std::vector<std::string> names;
    std::vector<std::string> errors;

    /// One instance per node that has a component, owned here.
    ///
    /// Keyed by NodeId, so a node being destroyed leaves an orphan that the
    /// next update sweeps - cheaper than the scene having to tell this module
    /// about every deletion, and this module is not the scene's business.
    std::unordered_map<NodeId, std::unique_ptr<Component>> instances;

    /// Which of them have had onStart called.
    std::unordered_set<NodeId> started;

    /// The name each instance was built from, so a node whose component is
    /// changed in the editor gets a new instance rather than keeping the old.
    std::unordered_map<NodeId, std::string> instanceNames;

    void clearInstances() {
        // Destroyed before the library is unloaded, and that order is not
        // optional: the code that knows how to destroy these objects is inside
        // the library, so unloading first would leave a vtable pointing at
        // memory the operating system has taken back.
        instances.clear();
        instanceNames.clear();
        started.clear();
    }
};

NativeEngine::NativeEngine(std::filesystem::path libraryDirectory)
    : m_state(std::make_unique<State>()) {
    m_state->directory = std::move(libraryDirectory);
}

NativeEngine::~NativeEngine() {
    // Same ordering rule as above, one last time.
    m_state->clearInstances();
}

u32 NativeEngine::reload(Scene& scene) {
    (void)scene;

    m_state->clearInstances();
    m_state->library.close();
    m_state->registry.factories.clear();
    m_state->names.clear();
    m_state->errors.clear();

    const std::filesystem::path built =
        m_state->directory / native::sharedLibraryFileName(kLibraryStem);

    std::error_code ec;
    if (!std::filesystem::exists(built, ec)) {
        // Not an error worth showing in red: a project that has not written any
        // C++ gameplay is in a perfectly normal state.
        FUMAR_DEBUG("no game library at '{}'", built.string());
        return 0;
    }

    const std::filesystem::path loaded =
        m_state->directory / native::sharedLibraryFileName(kLoadedStem);

    std::filesystem::copy_file(built, loaded, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        // Nearly always one thing: another fumar has the copy open. The name is
        // fixed, so two instances cannot both reload, and a crashed one that
        // never released the file looks identical. Worth naming, because the
        // operating system's own message says only that a file is in use and
        // leaves you to guess by what.
        m_state->errors.push_back(std::format(
            "could not replace '{}': {}. Another fumar is probably running and holding it.",
            loaded.filename().string(), ec.message()));
        return 0;
    }

    std::string error;
    if (!m_state->library.open(loaded, error)) {
        m_state->errors.push_back(std::format("could not load the game library: {}", error));
        return 0;
    }

    auto* registerComponents =
        reinterpret_cast<RegisterFunction>(m_state->library.symbol("fumarRegisterComponents"));
    if (registerComponents == nullptr) {
        m_state->errors.push_back(
            "the game library exports no fumarRegisterComponents - is it declared extern \"C\"?");
        m_state->library.close();
        return 0;
    }

    registerComponents(m_state->registry);

    m_state->names.reserve(m_state->registry.factories.size());
    for (const auto& [name, factory] : m_state->registry.factories) {
        m_state->names.push_back(name);
    }

    // Sorted, so the editor's list does not reorder itself between reloads -
    // an unordered_map has no opinion about order and will happily change its
    // mind.
    std::sort(m_state->names.begin(), m_state->names.end());

    FUMAR_INFO("game library loaded, {} component(s)", m_state->names.size());
    return static_cast<u32>(m_state->names.size());
}

bool NativeEngine::canRebuild() const {
#if defined(FUMAR_GAME_BUILD_DIR)
    std::error_code ec;
    return std::filesystem::exists(FUMAR_GAME_BUILD_DIR, ec);
#else
    return false;
#endif
}

bool NativeEngine::rebuild(Scene& scene) {
#if defined(FUMAR_GAME_BUILD_DIR)
    if (!canRebuild()) {
        m_state->errors.clear();
        m_state->errors.push_back(
            "no build directory - a distributed build has no compiler to rebuild with");
        return false;
    }

    // Unloaded FIRST. On Windows a loaded DLL cannot be replaced, and although
    // the build writes to the original rather than the copy, the linker also
    // wants the PDB beside it - which is locked too.
    m_state->clearInstances();
    m_state->library.close();

    FUMAR_INFO("rebuilding the game library");

    // Blocking. A half-written library must not be loaded, and there is nothing
    // useful to draw while waiting for a compiler anyway.
    const int result = std::system(buildCommand().c_str());

    if (result != 0) {
        m_state->errors.clear();
        m_state->errors.push_back(
            std::format("the build failed ({}). The compiler output is in the terminal fumar was "
                        "started from.",
                        result));
        return false;
    }

    return reload(scene) > 0;
#else
    (void)scene;
    m_state->errors.clear();
    m_state->errors.push_back("this build was configured without a game build command");
    return false;
#endif
}

void NativeEngine::update(Scene& scene, ScriptContext& context, f32 deltaSeconds) {
    if (m_state->registry.factories.empty()) {
        return;
    }

    const ComponentContext componentContext{
        .scene = scene,
        .script = context,
        .deltaSeconds = deltaSeconds,
    };

    // Collected first rather than walked directly, because a component is free
    // to create or delete nodes and that would invalidate an iteration in
    // progress.
    std::vector<NodeId> owners;
    scene.traverse([&](NodeId id, u32) {
        if (!scene.node(id).component.empty()) {
            owners.push_back(id);
        }
    });

    for (const NodeId id : owners) {
        if (!scene.isAlive(id)) {
            continue;
        }

        Node& node = scene.node(id);
        const std::string& name = node.component;

        // Rebuilt when the name changed, which is what makes picking a
        // different component in the details panel take effect immediately.
        const auto existingName = m_state->instanceNames.find(id);
        if (existingName != m_state->instanceNames.end() && existingName->second != name) {
            m_state->instances.erase(id);
            m_state->instanceNames.erase(id);
            m_state->started.erase(id);
        }

        if (!m_state->instances.contains(id)) {
            const auto factory = m_state->registry.factories.find(name);
            if (factory == m_state->registry.factories.end()) {
                continue;
            }

            Component* created = factory->second();
            if (created == nullptr) {
                continue;
            }
            m_state->instances.emplace(id, std::unique_ptr<Component>(created));
            m_state->instanceNames.emplace(id, name);
        }

        Component& component = *m_state->instances.at(id);

        if (m_state->started.insert(id).second) {
            component.onStart(node, componentContext);
        }

        // Re-checked: onStart is allowed to destroy the node it is on.
        if (scene.isAlive(id)) {
            component.onUpdate(scene.node(id), componentContext);
        }
    }

    // Sweep instances whose node is gone. Cheaper than the scene reporting
    // every deletion, and it keeps this module out of the scene's business.
    std::erase_if(m_state->instances, [&scene](const auto& entry) {
        return !scene.isAlive(entry.first);
    });
}

void NativeEngine::restart() {
    m_state->started.clear();
}

const std::vector<std::string>& NativeEngine::componentNames() const {
    return m_state->names;
}

const std::vector<std::string>& NativeEngine::errors() const {
    return m_state->errors;
}

bool NativeEngine::has(const std::string& name) const {
    return m_state->registry.factories.contains(name);
}

std::filesystem::path NativeEngine::projectDirectory() const {
#if defined(FUMAR_PROJECT_DIR)
    return FUMAR_PROJECT_DIR;
#else
    return {};
#endif
}

std::filesystem::path NativeEngine::sourceDirectory() const {
#if defined(FUMAR_GAME_SOURCE_DIR)
    return FUMAR_GAME_SOURCE_DIR;
#else
    return {};
#endif
}

bool NativeEngine::loaded() const {
    return m_state->library.valid();
}

} // namespace fumar
