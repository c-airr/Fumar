#pragma once

#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"

#include <array>
#include <functional>
#include <string_view>

namespace fumar {

/// Keys and buttons scripts can ask about.
///
/// A fixed enum rather than a passthrough of whatever the window system uses.
/// fumar_script deliberately does not depend on fumar_platform - the scripting
/// runtime should be drivable from a test with no window at all - so the set of
/// things a script can ask about is declared here and the application fills it
/// in from whatever it happens to be reading.
enum class ScriptKey : u8 {
    W,
    A,
    S,
    D,
    Q,
    E,
    Space,
    Shift,
    Control,
    Up,
    Down,
    Left,
    Right,
    MouseLeft,
    MouseRight,
    Count,
};

/// The name a script uses, or Count if it is not one we know.
ScriptKey scriptKeyFromName(std::string_view name);

/// What the scripts see of the keyboard and mouse this frame.
struct ScriptInput {
    std::array<bool, static_cast<usize>(ScriptKey::Count)> held{};

    /// Mouse movement since the last frame, in pixels. Only meaningful while
    /// the application is actually capturing the cursor.
    Vec2 mouseDelta{};

    bool down(ScriptKey key) const {
        return key != ScriptKey::Count && held[static_cast<usize>(key)];
    }
};

/// The camera, as a script may read and write it.
///
/// Not fumar::Camera, which lives in fumar_render and would drag the whole
/// renderer in behind it. The application copies the real camera in before an
/// update and copies it back out if `controlled` came back true - so a script
/// that never touches the camera leaves the editor's own fly camera alone,
/// and one that does takes it over completely.
struct ScriptCameraState {
    Vec3 position{};
    f32 yaw = 0.0f;
    f32 pitch = 0.0f;

    /// Set by the bindings the moment a script writes to the camera. This is
    /// what makes "the player script drives the view" a decision the script
    /// makes rather than a mode the editor has to be put into.
    bool controlled = false;
};

/// Everything a script can reach that is not the scene itself.
struct ScriptContext {
    ScriptInput input;
    ScriptCameraState camera;

    /// Traces a ray against the scene's geometry and returns the distance to
    /// the nearest hit, or a negative number for a miss.
    ///
    /// A callback rather than a method, because answering it needs mesh bounds,
    /// which live in the renderer's resource registry - and fumar_script must
    /// not depend on fumar_render. The application knows both and supplies the
    /// one function that joins them.
    ///
    /// Empty by default: a script calling it then simply always misses, which
    /// is the right behaviour for a scene with nothing to hit.
    std::function<f32(Vec3 origin, Vec3 direction, f32 maxDistance)> raycast;
};

} // namespace fumar
