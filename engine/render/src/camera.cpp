#include "fumar/render/camera.hpp"

#include "fumar/platform/window.hpp"

#include <cmath>

namespace fumar {

Vec3 Camera::forward() const {
    const f32 yawRad = radians(yaw);
    const f32 pitchRad = radians(pitch);
    const f32 cosPitch = std::cos(pitchRad);

    return normalize(Vec3{
        std::cos(yawRad) * cosPitch,
        std::sin(pitchRad),
        std::sin(yawRad) * cosPitch,
    });
}

Vec3 Camera::right() const {
    // Crossed against world up rather than the camera's own up, which keeps
    // strafing horizontal even when looking steeply up or down.
    return normalize(cross(forward(), Vec3{0.0f, 1.0f, 0.0f}));
}

Vec3 Camera::up() const {
    return normalize(cross(right(), forward()));
}

Mat4 Camera::view() const {
    return lookAt(position, position + forward(), Vec3{0.0f, 1.0f, 0.0f});
}

Mat4 Camera::projection(f32 aspect) const {
    return perspective(radians(fovYDegrees), aspect, nearPlane, farPlane);
}

void Camera::update(Window& window, f32 deltaSeconds) {
    // Holding the right mouse button grabs the cursor; Window releases it on
    // Escape. Grabbing on a held button rather than a click means there is no
    // edge to detect and no state to keep here.
    const bool wantsLook = window.mouseButtonDown(MouseButton::Right);
    if (wantsLook != window.relativeMouse()) {
        window.setRelativeMouse(wantsLook);
    }

    if (window.relativeMouse()) {
        const Vec2 delta = window.mouseDelta();
        yaw += delta.x * lookSensitivity;
        // Subtracted: moving the mouse down gives a positive Y delta, and
        // looking down means a smaller pitch.
        pitch -= delta.y * lookSensitivity;
        pitch = clamp(pitch, -89.0f, 89.0f);
    }

    // Accumulate a direction first and normalise once, so that moving diagonally
    // is not faster than moving straight - the classic bug where W+D outruns W.
    Vec3 direction{};
    if (window.keyDown(Key::W)) {
        direction += forward();
    }
    if (window.keyDown(Key::S)) {
        direction -= forward();
    }
    if (window.keyDown(Key::D)) {
        direction += right();
    }
    if (window.keyDown(Key::A)) {
        direction -= right();
    }
    if (window.keyDown(Key::Space)) {
        direction += Vec3{0.0f, 1.0f, 0.0f};
    }
    if (window.keyDown(Key::LeftControl)) {
        direction -= Vec3{0.0f, 1.0f, 0.0f};
    }

    if (lengthSquared(direction) > 0.0f) {
        const f32 speed = moveSpeed * (window.keyDown(Key::LeftShift) ? sprintMultiplier : 1.0f);
        // Scaling by deltaSeconds is what makes movement frame-rate
        // independent: at 30 fps each step is twice as large as at 60, so the
        // camera covers the same ground per second either way.
        position += normalize(direction) * (speed * deltaSeconds);
    }
}

} // namespace fumar
