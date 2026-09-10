#pragma once

#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"
#include "fumar/scene/scene.hpp"

#include <imgui.h>

#include <filesystem>
#include <string>

namespace fumar {

class ImGuiLayer;
class Renderer;
class ScriptEngine;
class Window;

/// What the gizmo in the viewport does when dragged.
enum class GizmoMode : u8 {
    Select,   ///< no handles; clicking picks objects
    Translate,
    Rotate,
    Scale,
};

/// Editor state that outlives a single frame.
///
/// Immediate mode means the interface itself keeps nothing: every panel is
/// described again each frame from the scene. What genuinely is editor state -
/// what is selected, which tool is active - lives here instead.
struct EditorState {
    NodeId selected = kInvalidNode;

    /// The node under the cursor in the viewport, refreshed by picking each
    /// frame. Purely visual feedback.
    NodeId hovered = kInvalidNode;

    GizmoMode gizmoMode = GizmoMode::Translate;

    /// Local space rotates and moves along the object own axes; world space
    /// along the scene axes. Scale is always local - scaling along a world axis
    /// on a rotated object would shear it.
    bool gizmoLocalSpace = false;

    /// Snapping increments, applied while the modifier key is held.
    bool snapEnabled = false;
    f32 translateSnap = 0.25f;
    f32 rotateSnap = 15.0f;
    f32 scaleSnap = 0.1f;

    /// Whether scripts run. Off by default so opening the editor does not
    /// immediately start moving things around, which would make placing an
    /// object by hand impossible.
    bool scriptsRunning = false;

    bool showOutliner = true;
    bool showWorld = true;
    bool showScripts = true;
    bool showDetails = true;
    bool showContent = true;
    bool showStats = true;
    bool showImGuiDemo = false;

    /// Frame time in milliseconds, smoothed for display. The raw value jitters
    /// too much to read.
    f32 smoothedFrameMs = 0.0f;

    /// Set from the menu to rebuild the default panel arrangement.
    bool resetLayoutRequested = false;

    // --- scene file ---------------------------------------------------------

    /// The file the scene was last saved to or loaded from. Empty means it has
    /// never been saved, and Save behaves as Save As.
    std::string scenePath;

    /// Requests raised by the menu and acted on by the main loop, rather than
    /// performed inside it. Loading a scene destroys the very nodes the panels
    /// are in the middle of describing, so it has to happen between frames.
    bool newSceneRequested = false;
    bool saveRequested = false;
    bool openRequested = false;
    std::string openPath;

    /// Shown briefly after a save, so the action is visibly acknowledged.
    f32 saveFlashSeconds = 0.0f;

    // --- viewport ----------------------------------------------------------

    /// The off-screen scene image, as ImGui knows it.
    ImTextureID viewportTexture = 0;

    /// Size of the viewport panel content, in pixels.
    Extent2D viewportSize{1280, 720};

    /// True while the cursor is over the viewport image and not over a gizmo.
    /// Picking and camera control both depend on it.
    bool viewportHovered = false;

    /// Cursor position inside the viewport, normalised to 0..1.
    Vec2 viewportCursor{0.0f, 0.0f};
};

/// Fills the whole window with an invisible dock host and draws the menu bar.
///
/// `sceneDirectory` is scanned for saved scenes to list under File > Open.
void drawDockspace(EditorState& state, const std::filesystem::path& sceneDirectory);

/// The scene image, the toolbar and the gizmo.
void drawViewportPanel(EditorState& state, Renderer& renderer, ScriptEngine& scripts);

/// The scene tree. Click to select, right-click for actions.
void drawOutlinerPanel(EditorState& state, Scene& scene);

/// Properties of whatever is selected.
///
/// Takes a mutable Renderer because materials are edited here: changing a
/// material affects every object using it, which is exactly how a material
/// asset is supposed to behave.
void drawDetailsPanel(EditorState& state, Scene& scene, Renderer& renderer,
                      const ScriptEngine& scripts);

/// The sun, the sky and the exposure - everything that lights the scene
/// without being in it.
void drawWorldPanel(EditorState& state, Renderer& renderer);

/// Meshes and materials in the project, and buttons to place them.
void drawContentPanel(EditorState& state, Renderer& renderer);

/// Frame timing and scene counts.
void drawStatsPanel(EditorState& state, const Scene& scene, const Renderer& renderer);

/// The script list, the Compile button and any compile errors.
void drawScriptsPanel(EditorState& state, ScriptEngine& scripts);

} // namespace fumar
