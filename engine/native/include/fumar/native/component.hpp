#pragma once

#include "fumar/core/types.hpp"
#include "fumar/scene/scene.hpp"
#include "fumar/script/script_context.hpp"

namespace fumar {

/// Everything a component can reach that is not the node it is attached to.
///
/// Deliberately the same set a Lua script gets - input, the camera, a way to
/// trace a ray - because the two are meant to be the same API from two
/// languages, not two different engines.
struct ComponentContext {
    Scene& scene;
    ScriptContext& script;
    f32 deltaSeconds;
};

/// Gameplay written in C++, attached to a node by name exactly as a Lua script
/// is.
///
/// State kept in a component DOES NOT SURVIVE A RELOAD. The whole object is
/// destroyed and rebuilt when the library is rebuilt, because the code that
/// knows how to destroy it is the code being replaced. That is not a wart to
/// work around - it is the rule that makes reloading safe, and the same rule
/// Unreal's hot reload lives by. Anything that has to persist belongs in the
/// scene, which the engine owns and which reloading never touches.
class Component {
public:
    virtual ~Component() = default;

    /// Called once, the first time this node updates after Play, a recompile,
    /// or a reload.
    virtual void onStart(Node& node, const ComponentContext& context) {
        (void)node;
        (void)context;
    }

    virtual void onUpdate(Node& node, const ComponentContext& context) = 0;
};

/// What the game library fills in when it is loaded.
///
/// An abstract interface implemented on the ENGINE side, and that is the point:
/// the library never touches an engine container. With a statically linked CRT
/// - which fumar uses so the executable needs no redistributable - the
/// executable and the library get separate heaps, and memory allocated by one
/// and freed by the other is a crash waiting for a quiet afternoon. Names cross
/// as const char*, and the only object that crosses is a Component, whose
/// virtual destructor sends the delete back into the library that made it.
class ComponentRegistry {
public:
    virtual ~ComponentRegistry() = default;

    /// A plain function pointer rather than std::function, for the same reason.
    using Factory = Component* (*)();

    virtual void add(const char* name, Factory factory) = 0;
};

} // namespace fumar

// ---------------------------------------------------------------------------
//  The single symbol the engine looks for in the game library.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#define FUMAR_GAME_EXPORT __declspec(dllexport)
#else
#define FUMAR_GAME_EXPORT __attribute__((visibility("default")))
#endif

/// Implemented once, in the game library. Called every time it is loaded.
///
/// extern "C" so the name is not mangled: the engine looks it up by string, and
/// a C++ mangled name differs between compilers and even between versions of
/// one.
extern "C" FUMAR_GAME_EXPORT void fumarRegisterComponents(fumar::ComponentRegistry& registry);

/// Registers one type under a name. `Type` must derive from fumar::Component.
///
/// The lambda has no captures, so it converts to a plain function pointer -
/// which is what keeps the boundary free of std::function.
#define FUMAR_REGISTER_COMPONENT(registry, Type)                                                   \
    (registry).add(#Type, []() -> fumar::Component* { return new Type(); })
