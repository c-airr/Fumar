#pragma once

#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"

namespace fumar {

enum class LightType : u8 {
    /// Shines equally in every direction from a point. A bare bulb.
    Point,

    /// A cone. Everything a point light is, plus a direction and two angles
    /// that say where the beam stops.
    Spot,
};

/// A light attached to a node.
///
/// The sun is NOT one of these - it lives in Environment, because a directional
/// light has no position and belongs to the sky rather than to the scene. These
/// are the lights you place.
///
/// Position and direction both come from the node's world transform rather than
/// being stored here: a light is then moved, rotated and parented by exactly
/// the same machinery as anything else, and a lamp attached to a car follows it
/// without a line of code about lights.
struct Light {
    LightType type = LightType::Point;

    Vec3 color{1.0f, 0.92f, 0.82f};

    /// Brightness at the source. Falls off with the square of the distance, so
    /// this number is larger than it looks: at three metres it is already down
    /// to a ninth.
    f32 intensity = 40.0f;

    /// Where the light is cut off entirely, in world units.
    ///
    /// Physically light never stops, it just becomes negligible - but a
    /// renderer that took that literally would test every light against every
    /// surface in the level. The range is what makes a light a local thing, and
    /// the falloff is shaped so it reaches zero smoothly at exactly this
    /// distance instead of ending in a visible edge.
    f32 range = 12.0f;

    /// Spot only: full brightness inside this angle from the axis, in degrees.
    f32 innerConeDegrees = 18.0f;

    /// Spot only: nothing outside this one. The gap between the two is the
    /// soft edge of the beam.
    f32 outerConeDegrees = 32.0f;

    /// Radius of the light source itself, in world units.
    ///
    /// Zero is a mathematical point, which casts shadows with edges infinitely
    /// sharp - something no real light does, because every real light has size.
    /// This is what the shadow rays are spread over.
    f32 sourceRadius = 0.12f;

    bool castsShadows = true;
};

} // namespace fumar
