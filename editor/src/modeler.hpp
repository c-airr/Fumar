#pragma once

#include "fumar/core/math.hpp"
#include "fumar/geometry/operations.hpp"
#include "fumar/geometry/raycast.hpp"
#include "fumar/render/camera.hpp"
#include "fumar/scene/scene.hpp"

#include <string>
#include <vector>

namespace fumar {

class Renderer;
struct EditorState;

/// Which half of the editor is showing.
///
/// Level is where a scene is put together - objects placed, scripts attached,
/// the game played. Modeler is where one object's shape is made. They share
/// the viewport, the outliner and the details, and differ in the panels around
/// them: the script editor and the content browser are noise while shaping a
/// mesh, and the modelling tools are noise while laying out a level.
enum class Workspace : u8 {
    Level,
    Modeler,
};

/// The geometry as it was before an edit, for undo inside a session.
struct EditSnapshot {
    geometry::EditableMesh mesh;
    geometry::Selection selection;
    geometry::SelectMode mode = geometry::SelectMode::Face;
};

/// What the cursor is over while a mesh is open.
struct EditHover {
    bool valid = false;
    u32 vertex = 0;
    geometry::EdgeKey edge;
    u32 face = 0;
};

/// One mesh open in the Modeler.
///
/// Opening a mesh gives the node a copy of its own - the other three blocks
/// that share the starter scene's cube are not changed by modelling one of
/// them - and that copy is what this edits and re-uploads as it changes. The
/// handle the node had before is kept, untouched, so that undoing at the
/// level of the scene puts the old shape back: the whole session is one step.
struct EditSession {
    NodeId node = kInvalidNode;
    MeshHandle mesh;
    MeshHandle originalMesh;

    geometry::EditableMesh geometry;
    geometry::Selection selection;
    geometry::SelectMode mode = geometry::SelectMode::Face;

    /// Undo and redo within the session. Whole copies of the mesh, the same
    /// trade as the scene history makes: at the size of anything modelled by
    /// hand, a copy costs less than the frame it is taken in.
    std::vector<EditSnapshot> undo;
    std::vector<EditSnapshot> redo;

    /// The level's camera, put back when the session ends. The Modeler frames
    /// the object on the way in, and leaving should not strand the view there.
    Camera levelCamera;

    /// Set when the geometry changed this frame. The upload to the GPU
    /// happens once, before the frame is drawn, however many times the mesh
    /// was touched on the way there.
    bool dirty = false;

    /// Bumped on every change, so derived data - which elements are hidden
    /// behind others - knows when it is stale.
    u64 version = 0;

    /// See through the mesh: select and draw elements on the far side as well.
    bool xray = false;

    f32 insetThickness = 0.1f;

    EditHover hover;

    /// The edge nearest the cursor whatever the mode, for Ctrl+R: a loop cut
    /// is aimed by pointing at an edge, even while faces are being selected.
    bool loopEdgeValid = false;
    geometry::EdgeKey loopEdge;

    /// A line of feedback in the tools panel - "inset needs faces" - and how
    /// long it stays.
    std::string status;
    f32 statusSeconds = 0.0f;

    // --- gizmo drag ----------------------------------------------------------
    // A drag moves the vertices from where they were when it BEGAN, by the
    // transform between the gizmo then and now. Accumulating small per-frame
    // deltas instead would drift - every frame's rounding error added to the
    // last - and turns a rotation into a slow spiral.
    bool dragging = false;
    Mat4 gizmoMatrix = identity();
    Mat4 dragStart = identity();
    std::vector<std::pair<u32, Vec3>> dragOrigins;

    // --- box selection -------------------------------------------------------
    bool pressed = false;
    bool boxSelecting = false;
    Vec2 boxStart{0.0f, 0.0f};

    // --- visibility cache ----------------------------------------------------
    // Which elements a ray from the camera reaches before anything else. Costs
    // a ray per element against every triangle, so it is only recomputed when
    // the camera or the mesh actually moved.
    std::vector<bool> vertexVisible;
    std::vector<bool> faceVisible;
    std::vector<std::pair<geometry::EdgeKey, bool>> edgeVisible;
    u64 visibilityVersion = ~0ull;
    Mat4 visibilityView{};
    bool visibilityXray = false;

    /// Rays against the mesh for picking faces and hiding what is behind.
    /// Rebuilt when the mesh changes, but not during a drag - the tree would
    /// be rebuilt every frame for a hover nobody is looking at.
    geometry::RayCaster caster;
    u64 casterVersion = ~0ull;
};

/// Opens the selected node's mesh for modelling. False if it has none.
bool enterEditSession(EditorState& state, Renderer& renderer);

/// Closes the open mesh, keeping what was made, and puts the scene back.
void exitEditSession(EditorState& state, Renderer& renderer);

/// Undo and redo inside the open mesh - what Ctrl+Z means while modelling.
void undoEdit(EditorState& state);
void redoEdit(EditorState& state);

/// Uploads the session's geometry if it changed this frame. Called once per
/// frame before drawing, so a drag that touches the mesh every frame costs one
/// upload a frame and not one per vertex.
void flushEditSession(EditorState& state, Renderer& renderer);

/// Tab, the selection modes, the operations and undo, while in the Modeler.
/// Returns true when it consumed the scene-level shortcuts too (undo, delete),
/// which then must not also act on the scene.
bool handleModelerShortcuts(EditorState& state, Renderer& renderer, bool navigating);

/// Everything the viewport does while a mesh is open: the overlay of vertices,
/// edges and faces, picking them, box selection, and the gizmo on the
/// selection. `origin` and `size` are the scene image's rectangle on screen.
///
/// Returns true while the gizmo is under the cursor or being dragged, so the
/// click is not also taken as a selection.
bool drawEditViewport(EditorState& state, Renderer& renderer, Vec2 origin, Vec2 size, bool imageHovered);

/// The modelling tools, where the script list is in the Level workspace.
void drawModelingToolsPanel(EditorState& state, Renderer& renderer);

/// The materials in the project, where the content browser is in the Level
/// workspace. Click one to put it on the selected object.
void drawMaterialsPanel(EditorState& state, Renderer& renderer);

/// Drives the Modeler without anyone at the keyboard, one step per call.
///
/// Opens Block A of the starter scene, extrudes its top, lifts it, insets it
/// and cuts a loop round its side - the same path a person takes, through the
/// same functions the shortcuts call - and logs the mesh's soundness after
/// each step. Variant "finish" then closes the session and undoes it at the
/// scene level, checking the cube comes back. Variant "helmet" opens the
/// starter scene's helmet instead and times it: the large mesh is where
/// anything quadratic would show. Variant "save" saves the scene after the
/// loop cut, loads it back and reopens the block, which is the round trip
/// through a .glb file.
///
/// Exists because the editor cannot be tested by synthesising key presses
/// on this machine, and the operations' own tests stop short of the GPU: this
/// is what exercises a mesh being replaced mid-flight under the validation
/// layers. Enabled by FUMAR_MODELER_SMOKE; returns false once it is done.
bool runModelerSmoke(EditorState& state, Renderer& renderer, u32 frame, const std::string& variant);

} // namespace fumar
