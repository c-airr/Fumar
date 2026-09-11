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

// One include for everything gameplay needs: the Component interface, the
// scene, maths, logging and input. The engine's own modules are split much more
// finely than this - for the engine's benefit, not yours.
#include <fumar.hpp>

namespace {

using namespace fumar;

/// Turns a node at a constant rate. The C++ twin of lua/spin.lua, kept
/// deliberately identical so the two can be compared line for line.
class Spinner final : public Component {
public:
    void onStart(Node& node, const ComponentContext&) override {
        FUMAR_INFO("[c++] Spinner started on {}", node.name);
    }

    void onUpdate(Node& node, const ComponentContext& context) override {
        // Multiplying by the frame time is what makes the speed the same
        // whatever the frame rate: at 30 fps each step is twice what it is at
        // 60, so the same ground is covered per second either way.
        const f32 turn = radians(m_degreesPerSecond * context.deltaSeconds);
        node.transform.rotation =
            normalize(fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, turn) * node.transform.rotation);
    }

private:
    f32 m_degreesPerSecond = 45.0f;
};

/// Moves a node up and down.
///
/// Worth reading for what it does NOT do: it keeps no phase counter. A member
/// holding accumulated time would be reset by every reload, and the node would
/// jump. Instead the height is derived from where the node already is, so the
/// scene carries the state and a reload changes nothing visible.
class Hover final : public Component {
public:
    void onStart(Node& node, const ComponentContext&) override {
        m_base = node.transform.position.y;
        m_elapsed = 0.0f;
    }

    void onUpdate(Node& node, const ComponentContext& context) override {
        m_elapsed += context.deltaSeconds;
        node.transform.position.y = m_base + std::sin(m_elapsed * m_speed) * m_amplitude;
    }

private:
    f32 m_base = 0.0f;
    f32 m_elapsed = 0.0f;
    f32 m_speed = 2.0f;
    f32 m_amplitude = 0.35f;
};

/// Follows whatever node is named, staying a fixed distance behind it.
///
/// Here to show the two things a component can reach beyond its own node: the
/// scene, to find another object, and the context, for the frame time.
class Follow final : public Component {
public:
    void onUpdate(Node& node, const ComponentContext& context) override {
        NodeId target = kInvalidNode;
        context.scene.traverse([&](NodeId id, u32) {
            if (target == kInvalidNode && context.scene.node(id).name == m_targetName) {
                target = id;
            }
        });

        if (target == kInvalidNode) {
            return;
        }

        const Vec3 wanted = context.scene.node(target).transform.position + m_offset;

        // Eased rather than snapped: moving a fraction of the remaining
        // distance each frame is the cheapest way to get a follow that does not
        // look mechanical. The frame time in the factor keeps it frame-rate
        // independent, approximately - exactly would want an exponential.
        const f32 factor = std::min(1.0f, m_stiffness * context.deltaSeconds);
        node.transform.position += (wanted - node.transform.position) * factor;
    }

private:
    std::string m_targetName = "Player";
    Vec3 m_offset{0.0f, 2.5f, 4.0f};
    f32 m_stiffness = 4.0f;
};

} // namespace

// The one symbol the engine looks for. Every component the editor should offer
// has to be named here - there is no reflection in C++ to find them otherwise.
extern "C" FUMAR_GAME_EXPORT void fumarRegisterComponents(fumar::ComponentRegistry& registry) {
    FUMAR_REGISTER_COMPONENT(registry, Spinner);
    FUMAR_REGISTER_COMPONENT(registry, Hover);
    FUMAR_REGISTER_COMPONENT(registry, Follow);
}
