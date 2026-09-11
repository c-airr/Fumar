// Gameplay in C++.
//
// This is the other half of fumar's scripting: the same on_start / on_update
// model as a Lua script, the same access to input, the camera and raycasts,
// compiled rather than interpreted. Press Compile in the editor and the library
// is rebuilt and swapped underneath the running scene.
//
// One rule, and everything else follows from it: MEMBER STATE DOES NOT SURVIVE
// A RELOAD. The object is destroyed and made again, because the code that knows
// how to destroy it is the code being replaced. Anything that has to persist
// goes in the scene - a node's transform, its name, its children - which the
// engine owns and reloading never touches.
//
// public/ holds what other files may include, private/ holds the implementation
// and anything only this file needs. With one pair it is a formality; the point
// is that it stays true as this grows, and the build already treats the two
// directories differently.

#include "components.h"

#include <cmath>

namespace game {

// --- Spinner ----------------------------------------------------------------

void Spinner::onStart(fumar::Node& node, const fumar::ComponentContext&) {
    FUMAR_INFO("[c++] Spinner started on {}", node.name);
}

void Spinner::onUpdate(fumar::Node& node, const fumar::ComponentContext& context) {
    // Multiplying by the frame time is what makes the speed the same whatever
    // the frame rate: at 30 fps each step is twice what it is at 60, so the
    // same ground is covered per second either way.
    const fumar::f32 turn = fumar::radians(m_degreesPerSecond * context.deltaSeconds);
    node.transform.rotation = fumar::normalize(
        fumar::fromAxisAngle(fumar::Vec3{0.0f, 1.0f, 0.0f}, turn) * node.transform.rotation);
}

// --- Hover ------------------------------------------------------------------

void Hover::onStart(fumar::Node& node, const fumar::ComponentContext&) {
    m_base = node.transform.position.y;
    m_elapsed = 0.0f;
}

void Hover::onUpdate(fumar::Node& node, const fumar::ComponentContext& context) {
    m_elapsed += context.deltaSeconds;
    node.transform.position.y = m_base + std::sin(m_elapsed * m_speed) * m_amplitude;
}

// --- Follow -----------------------------------------------------------------

void Follow::onUpdate(fumar::Node& node, const fumar::ComponentContext& context) {
    fumar::NodeId target = fumar::kInvalidNode;
    context.scene.traverse([&](fumar::NodeId id, fumar::u32) {
        if (target == fumar::kInvalidNode && context.scene.node(id).name == m_targetName) {
            target = id;
        }
    });

    if (target == fumar::kInvalidNode) {
        return;
    }

    const fumar::Vec3 wanted = context.scene.node(target).transform.position + m_offset;

    // Eased rather than snapped: moving a fraction of the remaining distance
    // each frame is the cheapest way to get a follow that does not look
    // mechanical. The frame time in the factor keeps it frame-rate independent,
    // approximately - exactly would want an exponential.
    const fumar::f32 factor = std::min(1.0f, m_stiffness * context.deltaSeconds);
    node.transform.position += (wanted - node.transform.position) * factor;
}

} // namespace game

// The one symbol the engine looks for. Every component the editor should offer
// has to be named here - C++ has no reflection to find them otherwise.
extern "C" FUMAR_GAME_EXPORT void fumarRegisterComponents(fumar::ComponentRegistry& registry) {
    using namespace game;

    FUMAR_REGISTER_COMPONENT(registry, Spinner);
    FUMAR_REGISTER_COMPONENT(registry, Hover);
    FUMAR_REGISTER_COMPONENT(registry, Follow);
}
