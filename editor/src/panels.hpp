#pragma once

#include "fumar/core/types.hpp"
#include "fumar/scene/scene.hpp"

namespace fumar {

class Renderer;

/// Editor state that outlives a single frame.
///
/// Immediate mode means the interface itself keeps nothing: every panel is
/// described again each frame from the scene. What genuinely is editor state -
/// what is selected, which panels are open - lives here instead.
struct EditorState {
    NodeId selected = kInvalidNode;

    bool showHierarchy = true;
    bool showInspector = true;
    bool showStats = true;
    bool showImGuiDemo = false;

    /// Frame time in milliseconds, smoothed for display. The raw value jitters
    /// too much to read.
    f32 smoothedFrameMs = 0.0f;

    /// Set from the menu to rebuild the default panel arrangement, discarding
    /// whatever the user had dragged into place.
    bool resetLayoutRequested = false;
};

/// Fills the whole window with an invisible dock host.
///
/// Panels dock against this, and its centre is left transparent so the scene
/// renders through it - that is the viewport until an off-screen render target
/// gives it a panel of its own.
void drawDockspace(EditorState& state);

/// The scene tree. Click to select, right-click for actions.
void drawHierarchyPanel(EditorState& state, Scene& scene);

/// Properties of whatever is selected.
void drawInspectorPanel(EditorState& state, Scene& scene, const Renderer& renderer);

/// Frame timing and scene counts.
void drawStatsPanel(EditorState& state, const Scene& scene, const Renderer& renderer);

} // namespace fumar
