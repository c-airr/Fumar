#pragma once

#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"

namespace fumar {

class Window;

/// A free-flying first-person camera.
///
/// Orientation is stored as yaw and pitch in degrees rather than a matrix or a
/// quaternion. For a camera that never rolls this is the simpler
/// representation: clamping pitch to keep the horizon level is one comparison,
/// where the equivalent on a quaternion is fiddly. Gimbal lock is not a concern
/// precisely because pitch is clamped away from straight up and down.
class Camera {
public:
    Vec3 position{0.0f, 1.5f, 4.0f};

    /// Rotation about the world Y axis, in degrees. -90 looks down -Z, which is
    /// the direction fumar treats as forward.
    f32 yaw = -90.0f;

    /// Rotation above and below the horizon, in degrees. Clamped to +/-89 so
    /// the view never flips over the top.
    f32 pitch = 0.0f;

    f32 fovYDegrees = 60.0f;
    f32 nearPlane = 0.1f;
    f32 farPlane = 500.0f;

    f32 moveSpeed = 4.0f;
    f32 sprintMultiplier = 4.0f;
    f32 lookSensitivity = 0.12f;

    /// Unit vector the camera is looking along.
    Vec3 forward() const;

    /// Unit vector pointing to the camera's right, always horizontal.
    Vec3 right() const;

    Vec3 up() const;

    /// World-to-camera transform.
    Mat4 view() const;

    /// Camera-to-clip transform, in Vulkan conventions (Y down, depth 0..1).
    Mat4 projection(f32 aspect) const;

    /// Applies mouse look and WASD movement for one frame.
    ///
    /// Takes the window by non-const reference because it also manages cursor
    /// capture: holding the right mouse button grabs the cursor, and Escape
    /// (handled inside Window) releases it. Movement is scaled by deltaSeconds
    /// so speed does not depend on frame rate.
    void update(Window& window, f32 deltaSeconds);
};

} // namespace fumar
