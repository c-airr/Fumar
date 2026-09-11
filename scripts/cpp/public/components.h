#pragma once

// One include for everything gameplay needs: the Component interface, the
// scene, maths, logging and input. The engine's own modules are split much more
// finely than this - for the engine's benefit, not yours.
#include <fumar.hpp>

#include <string>

/// Gameplay types live in their own namespace, so a component called Player
/// never has to argue with anything the engine happens to call Player.
namespace game {

/// Turns a node at a constant rate. The C++ twin of scripts/lua/spin.lua, kept
/// deliberately identical so the two can be compared line for line.
class Spinner final : public fumar::Component {
public:
    void onStart(fumar::Node& node, const fumar::ComponentContext& context) override;
    void onUpdate(fumar::Node& node, const fumar::ComponentContext& context) override;

private:
    fumar::f32 m_degreesPerSecond = 45.0f;
};

/// Moves a node up and down.
///
/// Worth reading for what it does NOT do: it keeps no phase counter across
/// reloads. A member holding accumulated time is reset every time the library
/// is rebuilt, so the height is re-derived from where the node already is -
/// the scene carries the state and a reload changes nothing visible.
class Hover final : public fumar::Component {
public:
    void onStart(fumar::Node& node, const fumar::ComponentContext& context) override;
    void onUpdate(fumar::Node& node, const fumar::ComponentContext& context) override;

private:
    fumar::f32 m_base = 0.0f;
    fumar::f32 m_elapsed = 0.0f;
    fumar::f32 m_speed = 2.0f;
    fumar::f32 m_amplitude = 0.35f;
};

/// Follows whatever node is named, staying a fixed distance behind it.
///
/// Here to show the two things a component can reach beyond its own node: the
/// scene, to find another object, and the context, for the frame time.
class Follow final : public fumar::Component {
public:
    void onUpdate(fumar::Node& node, const fumar::ComponentContext& context) override;

private:
    std::string m_targetName = "Player";
    fumar::Vec3 m_offset{0.0f, 2.5f, 4.0f};
    fumar::f32 m_stiffness = 4.0f;
};

} // namespace game
