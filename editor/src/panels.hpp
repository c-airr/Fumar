#pragma once

#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"
#include "fumar/scene/scene.hpp"

#include "history.hpp"

#include <imgui.h>

#include <filesystem>
#include <string>
#include <vector>

namespace fumar {

class ImGuiLayer;
class Renderer;
class NativeEngine;
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

    /// Undo, redo, and the clipboard - all three are the same mechanism, a
    /// snapshot of the scene taken before a change. Lives here so any panel can
    /// record a step without the history being threaded through every call.
    History history;
    SceneSnapshot clipboard;

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

    /// The scene, for the handful of panel actions that need it but are not
    /// handed it - rebuilding the game library has to drop every component
    /// instance, and those live per node.
    Scene* sceneForRebuild = nullptr;

    /// Frames left to keep asking for Content to be the open tab.
    ///
    /// Which tab of a docked group is in front is decided by which window was
    /// focused last, and on a fresh layout that is simply whichever panel calls
    /// Begin() last. Asking for a few frames after the layout is built is what
    /// makes the choice deliberate; one frame is not enough, because the
    /// docking node is still settling.
    i32 focusContentFrames = 0;

    // --- edit actions -------------------------------------------------------
    // Raised by the menu or by a shortcut and acted on in one place afterwards,
    // rather than performed where they are triggered. Undo and paste both
    // destroy and create nodes, and doing that while the outliner is halfway
    // through describing the tree would leave ImGui holding freed strings.
    bool undoRequested = false;
    bool redoRequested = false;
    bool copyRequested = false;
    bool pasteRequested = false;
    bool duplicateRequested = false;
    bool deleteRequested = false;

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

    // --- script editor ------------------------------------------------------

    /// The script file open in the editor, or empty for none.
    std::string openScript;

    /// Its text, edited in place.
    ///
    /// A fixed buffer rather than a std::string bound through ImGui's resize
    /// callback. Scripts are source files - a long one is a few thousand
    /// characters - so the ceiling is never reached in practice, and this is
    /// one less moving part in a text field that is already the most stateful
    /// widget in the editor.
    static constexpr usize kScriptBufferSize = 32 * 1024;
    std::vector<char> scriptBuffer = std::vector<char>(kScriptBufferSize, '\0');

    /// True once the text differs from what is on disk.
    bool scriptDirty = false;

    /// Name being typed into the "new script" field.
    std::string newScriptName;

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
void drawViewportPanel(EditorState& state, Renderer& renderer, ScriptEngine& scripts,
                       NativeEngine& native);

/// The scene tree. Click to select, right-click for actions.
///
/// Takes the Renderer because the Add menu lives here and creating a cube needs
/// a mesh uploaded to the GPU.
void drawOutlinerPanel(EditorState& state, Renderer& renderer);

/// Properties of whatever is selected.
///
/// Takes a mutable Renderer because materials are edited here: changing a
/// material affects every object using it, which is exactly how a material
/// asset is supposed to behave.
void drawDetailsPanel(EditorState& state, Scene& scene, Renderer& renderer,
                      const ScriptEngine& scripts, const NativeEngine& native);

/// The sun, the sky and the exposure. Drawn INSIDE the details panel when
/// nothing is selected, rather than as a panel of its own: an inspector with
/// nothing to inspect is wasted space, and the world is what you are looking at
/// when you are not looking at an object.
void drawWorldSettings(Renderer& renderer);

/// Meshes and materials in the project, and buttons to place them.
void drawContentPanel(EditorState& state, Renderer& renderer);

/// Frame timing and scene counts.
void drawStatsPanel(EditorState& state, const Scene& scene, const Renderer& renderer);

/// The script list, the Compile button and any compile errors.
void drawScriptsPanel(EditorState& state, ScriptEngine& scripts, NativeEngine& native);

} // namespace fumar
