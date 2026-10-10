#include "modeler.hpp"

#include "panels.hpp"

#include "fumar/core/log.hpp"
#include "fumar/platform/paths.hpp"
#include "fumar/render/renderer.hpp"
#include "fumar/render/scene_io.hpp"
#include "fumar/ui/imgui_layer.hpp"

#include <ImGuizmo.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace fumar {
namespace {

namespace geo = geometry;

constexpr usize kMaxUndo = 128;

/// How close, in pixels, the cursor has to be to a vertex or an edge for it to
/// count as under the cursor. Generous on purpose: a vertex is a point, and
/// asking anyone to land on a point is asking them to miss.
constexpr f32 kVertexPickPixels = 10.0f;
constexpr f32 kEdgePickPixels = 8.0f;
constexpr f32 kLoopEdgePickPixels = 24.0f;

enum class Operation : u8 {
    Extrude,
    Inset,
    LoopCut,
    Delete,
    Merge,
    Fill,
    Subdivide,
    FlipNormals,
};

/// A draw-list colour. Through uiColor like every other colour in the editor:
/// the window is presented in sRGB, so a value written straight out would come
/// back lighter than it was picked.
ImU32 colour(f32 r, f32 g, f32 b, f32 a = 1.0f) { return ImGui::GetColorU32(uiColor(r, g, b, a)); }

ImU32 accent(f32 a = 1.0f) { return colour(0.659f, 0.373f, 0.173f, a); }
ImU32 accentBright(f32 a = 1.0f) { return colour(1.0f, 0.72f, 0.45f, a); }

ImVec2 imVec(Vec2 v) { return ImVec2(v.x, v.y); }

void setStatus(EditSession& session, std::string text) {
    session.status = std::move(text);
    session.statusSeconds = 3.0f;
}

void markChanged(EditSession& session) {
    session.dirty = true;
    ++session.version;
}

void pushUndo(EditSession& session) {
    session.undo.push_back(EditSnapshot{session.geometry, session.selection, session.mode});
    if (session.undo.size() > kMaxUndo) {
        session.undo.erase(session.undo.begin());
    }
    session.redo.clear();
}

/// Everything needed to turn a point of the mesh into a pixel and a pixel into
/// a ray: the object's transform, the camera, and where the image is on screen.
struct Projector {
    Mat4 world = identity();
    Mat4 toObject = identity();
    Mat4 viewProjection = identity();
    Vec2 origin{0.0f, 0.0f};
    Vec2 size{1.0f, 1.0f};

    /// The camera, in the mesh's own space. Rays for visibility start here and
    /// end at the element being tested, so they are in the same space as the
    /// mesh and need no transforming per triangle.
    Vec3 eye{0.0f, 0.0f, 0.0f};

    bool toScreen(Vec3 local, Vec2& screen) const {
        const Vec4 clip = viewProjection * (world * point(local));
        if (clip.w <= 1e-4f) {
            return false; // behind the camera
        }
        // fumar's projection already flips Y for Vulkan, so normalised device
        // Y runs downward - the same way as screen pixels.
        screen = Vec2{origin.x + (clip.x / clip.w * 0.5f + 0.5f) * size.x,
                      origin.y + (clip.y / clip.w * 0.5f + 0.5f) * size.y};
        return true;
    }
};

Projector makeProjector(const EditSession& session, const Renderer& renderer, Vec2 origin, Vec2 size) {
    Projector projector;
    const f32 aspect = size.y > 0.0f ? size.x / size.y : 1.0f;
    projector.world = renderer.scene().worldTransform(session.node);
    projector.toObject = inverse(projector.world);
    projector.viewProjection = renderer.camera().projection(aspect) * renderer.camera().view();
    projector.origin = origin;
    projector.size = size;
    projector.eye = xyz(projector.toObject * point(renderer.camera().position));
    return projector;
}

f32 distanceToSegment(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const f32 lengthSq = dot(ab, ab);
    const f32 t = lengthSq > 0.0f ? std::clamp(dot(p - a, ab) / lengthSq, 0.0f, 1.0f) : 0.0f;
    return length(p - (a + ab * t));
}

void refreshCaster(EditSession& session) {
    if (session.casterVersion != session.version && !session.dragging) {
        session.caster = geo::RayCaster(session.geometry);
        session.casterVersion = session.version;
    }
}

/// Which vertices, edges and faces the camera can see, by casting a ray from
/// the eye to each one and asking whether the mesh is in the way.
///
/// Recomputed only when the camera or the mesh moved. During a gizmo drag the
/// answer from before the drag is kept: the mesh changes every frame then, and
/// nobody is choosing what to select while dragging.
void refreshVisibility(EditSession& session, const Renderer& renderer, const Projector& projector) {
    const Mat4 view = renderer.camera().view();
    const bool sameView = std::memcmp(&view, &session.visibilityView, sizeof(Mat4)) == 0;
    const bool sizesMatch = session.vertexVisible.size() == session.geometry.positions.size() &&
                            session.faceVisible.size() == session.geometry.faces.size();
    if (sizesMatch && sameView && session.visibilityXray == session.xray &&
        (session.visibilityVersion == session.version || session.dragging)) {
        return;
    }

    refreshCaster(session);
    const geo::EditableMesh& mesh = session.geometry;
    const Vec3 eye = projector.eye;

    // Aimed at the element with t = 1 there, and stopping just short of it so
    // the faces the element itself belongs to do not count as in the way.
    const auto visible = [&](Vec3 target) {
        return session.xray || !session.caster.occluded(eye, target - eye, 0.995f);
    };

    session.vertexVisible.assign(mesh.positions.size(), false);
    for (usize v = 0; v < mesh.positions.size(); ++v) {
        session.vertexVisible[v] = visible(mesh.positions[v]);
    }

    session.faceVisible.assign(mesh.faces.size(), false);
    for (usize f = 0; f < mesh.faces.size(); ++f) {
        session.faceVisible[f] = visible(geo::faceCentroid(mesh, mesh.faces[f]));
    }

    session.edgeVisible.clear();
    for (const geo::Edge& edge : mesh.buildEdges().edges) {
        const Vec3 middle = (mesh.positions[edge.key.a] + mesh.positions[edge.key.b]) * 0.5f;
        session.edgeVisible.emplace_back(edge.key, visible(middle));
    }

    session.visibilityVersion = session.version;
    session.visibilityView = view;
    session.visibilityXray = session.xray;
}

/// The same elements, seen from the selection mode being switched to: faces
/// whose corners were all selected, edges whose two ends were.
geo::Selection convertSelection(const geo::EditableMesh& mesh, const geo::Selection& selection,
                                geo::SelectMode from, geo::SelectMode to) {
    const std::set<u32> covered = geo::coveredVertices(mesh, selection, from);
    geo::Selection result;
    switch (to) {
    case geo::SelectMode::Vertex:
        result.vertices = covered;
        break;
    case geo::SelectMode::Edge:
        for (const geo::Edge& edge : mesh.buildEdges().edges) {
            if (covered.contains(edge.key.a) && covered.contains(edge.key.b)) {
                result.edges.insert(edge.key);
            }
        }
        break;
    case geo::SelectMode::Face:
        for (u32 f = 0; f < mesh.faces.size(); ++f) {
            const auto& verts = mesh.faces[f].verts;
            if (std::all_of(verts.begin(), verts.end(), [&](u32 v) { return covered.contains(v); })) {
                result.faces.insert(f);
            }
        }
        break;
    }
    return result;
}

void setMode(EditSession& session, geo::SelectMode mode) {
    if (session.mode == mode) {
        return;
    }
    session.selection = convertSelection(session.geometry, session.selection, session.mode, mode);
    session.mode = mode;
}

void perform(EditorState& state, Operation operation) {
    if (!state.edit.has_value()) {
        return;
    }
    EditSession& session = *state.edit;

    const bool needsSelection = operation != Operation::LoopCut;
    if (needsSelection && session.selection.empty()) {
        setStatus(session, "Select something first.");
        return;
    }

    pushUndo(session);
    geo::Selection result;
    bool changed = true;

    switch (operation) {
    case Operation::Extrude:
        result = geo::extrude(session.geometry, session.selection, session.mode);
        // Straight to Move, so the very next drag pulls the new geometry out.
        // That is the whole point of extruding, and it saves pressing W.
        state.gizmoMode = GizmoMode::Translate;
        changed = !result.empty();
        break;

    case Operation::Inset:
        if (session.mode != geo::SelectMode::Face) {
            setStatus(session, "Inset works on faces (3).");
            changed = false;
            break;
        }
        result = geo::inset(session.geometry, session.selection, session.insetThickness);
        changed = !result.empty();
        break;

    case Operation::LoopCut: {
        geo::EdgeKey start;
        bool aimed = false;
        if (session.mode == geo::SelectMode::Edge && session.selection.edges.size() == 1) {
            start = *session.selection.edges.begin();
            aimed = true;
        } else if (session.loopEdgeValid) {
            start = session.loopEdge;
            aimed = true;
        }
        if (!aimed) {
            setStatus(session, "Point at an edge, then Ctrl+R.");
            changed = false;
            break;
        }
        result = geo::loopCut(session.geometry, start);
        if (result.empty()) {
            setStatus(session, "No ring of quads runs through that edge.");
            changed = false;
        } else {
            session.mode = geo::SelectMode::Edge;
        }
        break;
    }

    case Operation::Delete:
        geo::deleteSelection(session.geometry, session.selection, session.mode);
        break;

    case Operation::Merge:
        result = geo::mergeAtCentre(session.geometry, session.selection, session.mode);
        session.mode = geo::SelectMode::Vertex;
        break;

    case Operation::Fill:
        result = geo::fill(session.geometry, session.selection, session.mode);
        if (result.empty()) {
            setStatus(session, "Fill needs two edges, a closed loop, or three vertices.");
            changed = false;
        } else {
            session.mode = geo::SelectMode::Face;
        }
        break;

    case Operation::Subdivide:
        if (session.mode != geo::SelectMode::Face) {
            setStatus(session, "Subdivide works on faces (3).");
            changed = false;
            break;
        }
        result = geo::subdivide(session.geometry, session.selection);
        break;

    case Operation::FlipNormals:
        if (session.mode != geo::SelectMode::Face) {
            setStatus(session, "Flip works on faces (3).");
            changed = false;
            break;
        }
        geo::flipNormals(session.geometry, session.selection);
        result = session.selection;
        break;
    }

    if (!changed) {
        session.undo.pop_back();
        return;
    }

    session.selection = std::move(result);
    markChanged(session);

#if !defined(NDEBUG)
    // Every operation is covered by tools/geometry_tests, but a mesh imported
    // from elsewhere can start out in a shape the tests never built. Saying so
    // here is what turns "the extrude looks wrong" into a line in the log.
    for (const std::string& problem : geo::validate(session.geometry)) {
        FUMAR_WARN("mesh after edit: {}", problem);
    }
#endif
}

/// The selection pivot's frame: at the middle of the selection, along the
/// object's own axes or the world's.
Mat4 pivotFrame(const Projector& projector, Vec3 pivotLocal, bool localAxes) {
    Mat4 frame = translation(xyz(projector.world * point(pivotLocal)));
    if (localAxes) {
        // The object's rotation without its scale - a gizmo stretched by a
        // non-uniform scale would drag in skewed directions.
        Mat4 rotationOnly = identity();
        for (usize c = 0; c < 3; ++c) {
            const Vec3 axis = xyz(projector.world.columns[c]);
            const f32 len = length(axis);
            rotationOnly.columns[c] = direction(len > 0.0f ? axis / len : axis);
        }
        frame = frame * rotationOnly;
    }
    return frame;
}

/// The gizmo on the selection. Returns true while it is under the cursor or in
/// use.
bool manipulateSelection(EditorState& state, const Renderer& renderer, const Projector& projector) {
    EditSession& session = *state.edit;
    if (state.gizmoMode == GizmoMode::Select) {
        session.dragging = false;
        return false;
    }

    const std::set<u32> covered = geo::coveredVertices(session.geometry, session.selection, session.mode);
    if (covered.empty()) {
        session.dragging = false;
        return false;
    }

    Vec3 pivot{0.0f, 0.0f, 0.0f};
    for (const u32 v : covered) {
        pivot += session.geometry.positions[v];
    }
    pivot = pivot / static_cast<f32>(covered.size());

    if (!session.dragging) {
        session.gizmoMatrix = pivotFrame(projector, pivot, state.gizmoLocalSpace);
    }

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(projector.origin.x, projector.origin.y, projector.size.x, projector.size.y);

    const f32 aspect = projector.size.y > 0.0f ? projector.size.x / projector.size.y : 1.0f;
    const Mat4 view = renderer.camera().view();
    Mat4 projection = renderer.camera().projection(aspect);
    // Same as the object gizmo: ImGuizmo expects OpenGL's Y-up projection.
    projection.at(1, 1) = -projection.at(1, 1);

    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    f32 snapValue = state.translateSnap;
    if (state.gizmoMode == GizmoMode::Rotate) {
        operation = ImGuizmo::ROTATE;
        snapValue = state.rotateSnap;
    } else if (state.gizmoMode == GizmoMode::Scale) {
        operation = ImGui::GetIO().KeyShift ? ImGuizmo::SCALEU : ImGuizmo::SCALE;
        snapValue = state.scaleSnap;
    }
    const f32 snap[3]{snapValue, snapValue, snapValue};

    // LOCAL always: the frame built above already holds the axes wanted, and
    // scaling is only ever along a frame's own axes in ImGuizmo.
    const Mat4 before = session.gizmoMatrix;
    ImGuizmo::Manipulate(&view.columns[0].x, &projection.columns[0].x, operation, ImGuizmo::LOCAL,
                         &session.gizmoMatrix.columns[0].x, nullptr, state.snapEnabled ? snap : nullptr);

    if (ImGuizmo::IsUsing()) {
        if (!session.dragging) {
            session.dragging = true;
            pushUndo(session);
            session.dragStart = before;
            session.dragOrigins.clear();
            for (const u32 v : covered) {
                session.dragOrigins.emplace_back(v, session.geometry.positions[v]);
            }
        }

        // The whole drag as one transform, from where the gizmo started to
        // where it is now, carried into the object's space: out of it, moved,
        // and back in.
        const Mat4 moved = projector.toObject * session.gizmoMatrix * inverse(session.dragStart) * projector.world;
        for (const auto& [v, origin] : session.dragOrigins) {
            session.geometry.positions[v] = xyz(moved * point(origin));
        }
        markChanged(session);
    } else {
        session.dragging = false;
    }

    return ImGuizmo::IsOver() || ImGuizmo::IsUsing();
}

/// What is under the cursor, in the current mode, plus the nearest edge for a
/// loop cut.
void updateHover(EditSession& session, const Renderer& renderer, const Projector& projector, Vec2 mouse,
                 bool active) {
    session.hover = EditHover{};
    session.loopEdgeValid = false;
    if (!active || session.dragging) {
        return;
    }

    const geo::EditableMesh& mesh = session.geometry;

    // The nearest edge, whatever the mode - Ctrl+R aims with it.
    f32 bestEdge = kLoopEdgePickPixels;
    for (const auto& [key, visible] : session.edgeVisible) {
        Vec2 a;
        Vec2 b;
        if (!visible || !projector.toScreen(mesh.positions[key.a], a) || !projector.toScreen(mesh.positions[key.b], b)) {
            continue;
        }
        const f32 d = distanceToSegment(mouse, a, b);
        if (d < bestEdge) {
            bestEdge = d;
            session.loopEdge = key;
            session.loopEdgeValid = true;
        }
    }

    switch (session.mode) {
    case geo::SelectMode::Vertex: {
        f32 best = kVertexPickPixels;
        for (u32 v = 0; v < mesh.positions.size(); ++v) {
            Vec2 screen;
            if (!session.vertexVisible[v] || !projector.toScreen(mesh.positions[v], screen)) {
                continue;
            }
            const f32 d = length(screen - mouse);
            if (d < best) {
                best = d;
                session.hover.valid = true;
                session.hover.vertex = v;
            }
        }
        break;
    }
    case geo::SelectMode::Edge:
        if (session.loopEdgeValid && bestEdge <= kEdgePickPixels) {
            session.hover.valid = true;
            session.hover.edge = session.loopEdge;
        }
        break;
    case geo::SelectMode::Face: {
        refreshCaster(session);
        const f32 aspect = projector.size.y > 0.0f ? projector.size.x / projector.size.y : 1.0f;
        const Vec2 normalised{(mouse.x - projector.origin.x) / projector.size.x,
                              (mouse.y - projector.origin.y) / projector.size.y};
        const Ray ray = renderer.camera().rayThrough(normalised, aspect);
        const Vec3 origin = xyz(projector.toObject * point(ray.origin));
        const Vec3 along = xyz(projector.toObject * direction(ray.direction));
        if (const auto hit = session.caster.closest(origin, along)) {
            session.hover.valid = true;
            session.hover.face = hit->face;
        }
        break;
    }
    }
}

void clickSelect(EditSession& session, bool additive) {
    if (!session.hover.valid) {
        if (!additive) {
            session.selection.clear();
        }
        return;
    }

    const auto toggle = [additive, &session](auto& set, const auto& element) {
        if (!additive) {
            session.selection.clear();
            set.insert(element);
        } else if (!set.erase(element)) {
            set.insert(element);
        }
    };

    switch (session.mode) {
    case geo::SelectMode::Vertex:
        toggle(session.selection.vertices, session.hover.vertex);
        break;
    case geo::SelectMode::Edge:
        toggle(session.selection.edges, session.hover.edge);
        break;
    case geo::SelectMode::Face:
        toggle(session.selection.faces, session.hover.face);
        break;
    }
}

void boxSelect(EditSession& session, const Projector& projector, Vec2 cornerA, Vec2 cornerB, bool additive) {
    const Vec2 low{std::min(cornerA.x, cornerB.x), std::min(cornerA.y, cornerB.y)};
    const Vec2 high{std::max(cornerA.x, cornerB.x), std::max(cornerA.y, cornerB.y)};
    const auto inside = [&](Vec3 local) {
        Vec2 s;
        return projector.toScreen(local, s) && s.x >= low.x && s.x <= high.x && s.y >= low.y && s.y <= high.y;
    };

    if (!additive) {
        session.selection.clear();
    }

    const geo::EditableMesh& mesh = session.geometry;
    switch (session.mode) {
    case geo::SelectMode::Vertex:
        for (u32 v = 0; v < mesh.positions.size(); ++v) {
            if (session.vertexVisible[v] && inside(mesh.positions[v])) {
                session.selection.vertices.insert(v);
            }
        }
        break;
    case geo::SelectMode::Edge:
        for (const auto& [key, visible] : session.edgeVisible) {
            if (visible && inside((mesh.positions[key.a] + mesh.positions[key.b]) * 0.5f)) {
                session.selection.edges.insert(key);
            }
        }
        break;
    case geo::SelectMode::Face:
        for (u32 f = 0; f < mesh.faces.size(); ++f) {
            if (session.faceVisible[f] && inside(geo::faceCentroid(mesh, mesh.faces[f]))) {
                session.selection.faces.insert(f);
            }
        }
        break;
    }
}

void drawOverlay(const EditSession& session, const Projector& projector) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(imVec(projector.origin), imVec(projector.origin + projector.size), true);

    const geo::EditableMesh& mesh = session.geometry;
    const geo::Selection& selection = session.selection;

    // --- faces: a tint over the selected ones, a faint one under the cursor
    std::vector<ImVec2> corners;
    const auto fillFace = [&](u32 f, ImU32 fill) {
        corners.clear();
        for (const u32 v : mesh.faces[f].verts) {
            Vec2 s;
            if (!projector.toScreen(mesh.positions[v], s)) {
                return;
            }
            corners.push_back(imVec(s));
        }
        draw->AddConvexPolyFilled(corners.data(), static_cast<int>(corners.size()), fill);
    };

    if (session.mode == geo::SelectMode::Face) {
        for (const u32 f : selection.faces) {
            if (f < mesh.faces.size() && (session.xray || session.faceVisible[f])) {
                fillFace(f, accent(0.35f));
            }
        }
        if (session.hover.valid && session.hover.face < mesh.faces.size()) {
            fillFace(session.hover.face, colour(1.0f, 1.0f, 1.0f, 0.10f));
        }
    }

    // --- edges: dark wire, the selected ones in the accent
    std::set<geo::EdgeKey> faceEdges;
    if (session.mode == geo::SelectMode::Face) {
        for (const u32 f : selection.faces) {
            if (f >= mesh.faces.size()) {
                continue;
            }
            const auto& verts = mesh.faces[f].verts;
            for (usize i = 0; i < verts.size(); ++i) {
                faceEdges.insert(geo::EdgeKey{verts[i], verts[(i + 1) % verts.size()]});
            }
        }
    }

    const ImU32 wire = colour(0.02f, 0.02f, 0.02f, 0.85f);
    for (const auto& [key, visible] : session.edgeVisible) {
        if (!visible) {
            continue;
        }
        Vec2 a;
        Vec2 b;
        if (!projector.toScreen(mesh.positions[key.a], a) || !projector.toScreen(mesh.positions[key.b], b)) {
            continue;
        }

        bool selected = false;
        switch (session.mode) {
        case geo::SelectMode::Vertex:
            selected = selection.vertices.contains(key.a) && selection.vertices.contains(key.b);
            break;
        case geo::SelectMode::Edge:
            selected = selection.edges.contains(key);
            break;
        case geo::SelectMode::Face:
            selected = faceEdges.contains(key);
            break;
        }

        const bool hovered = session.mode == geo::SelectMode::Edge && session.hover.valid && session.hover.edge == key;
        if (hovered) {
            draw->AddLine(imVec(a), imVec(b), colour(1.0f, 1.0f, 1.0f, 0.9f), 3.0f);
        } else if (selected) {
            draw->AddLine(imVec(a), imVec(b), accentBright(), 2.5f);
        } else {
            draw->AddLine(imVec(a), imVec(b), wire, 1.2f);
        }
    }

    // --- vertices: only in vertex mode, where they are what gets clicked
    if (session.mode == geo::SelectMode::Vertex) {
        for (u32 v = 0; v < mesh.positions.size(); ++v) {
            Vec2 s;
            if (!session.vertexVisible[v] || !projector.toScreen(mesh.positions[v], s)) {
                continue;
            }
            if (selection.vertices.contains(v)) {
                draw->AddCircleFilled(imVec(s), 4.0f, accentBright());
            } else {
                draw->AddCircleFilled(imVec(s), 3.0f, colour(0.02f, 0.02f, 0.02f, 0.9f));
            }
        }
        if (session.hover.valid) {
            Vec2 s;
            if (projector.toScreen(mesh.positions[session.hover.vertex], s)) {
                draw->AddCircle(imVec(s), 7.0f, colour(1.0f, 1.0f, 1.0f, 0.9f), 0, 2.0f);
            }
        }
    }

    // --- where Ctrl+R would cut, while it is held
    if (ImGui::GetIO().KeyCtrl && session.loopEdgeValid) {
        Vec2 a;
        Vec2 b;
        if (projector.toScreen(mesh.positions[session.loopEdge.a], a) &&
            projector.toScreen(mesh.positions[session.loopEdge.b], b)) {
            draw->AddLine(imVec(a), imVec(b), colour(0.9f, 0.85f, 0.3f, 0.9f), 3.0f);
        }
    }

    draw->PopClipRect();
}

/// A button that looks pressed while its mode is the active one.
bool modeButton(const char* label, bool active, const char* tooltip, f32 width) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, uiColor(0.659f, 0.373f, 0.173f));
    }
    const bool pressed = ImGui::Button(label, ImVec2(width, 0.0f));
    if (active) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return pressed;
}

/// Puts the camera `orbitDistance` back from the target along the way it
/// already looks. Everything that moves the Modeler's camera changes the
/// target, the distance or the angle and then calls this, so the three can
/// never disagree.
void placeOrbitCamera(const EditSession& session, Camera& camera) {
    camera.position = session.orbitTarget - camera.forward() * session.orbitDistance;
}

/// Aims the camera at some of the mesh's vertices - all of them when
/// `vertices` is empty - and backs off until their bounding sphere fits the
/// view, with `margin` to spare.
void frameVertices(EditSession& session, const Renderer& renderer, const std::set<u32>& vertices, f32 margin) {
    const auto& positions = session.geometry.positions;
    if (positions.empty()) {
        return;
    }
    const Mat4& world = renderer.scene().worldTransform(session.node);
    Vec3 low{1e30f, 1e30f, 1e30f};
    Vec3 high{-1e30f, -1e30f, -1e30f};
    const auto include = [&](u32 v) {
        const Vec3 w = xyz(world * point(positions[v]));
        low = min(low, w);
        high = max(high, w);
    };
    if (vertices.empty()) {
        for (u32 v = 0; v < positions.size(); ++v) {
            include(v);
        }
    } else {
        for (const u32 v : vertices) {
            include(v);
        }
    }

    // A floor on the radius, or one vertex would put the camera inside it.
    const f32 radius = std::max(length(high - low) * 0.5f, 0.1f);
    const f32 halfFov = radians(renderer.camera().fovYDegrees) * 0.5f;
    session.orbitTarget = (low + high) * 0.5f;
    session.orbitDistance = radius / std::sin(halfFov) * margin;
}

/// Puts a panel into the same dock node as another one, the first time it is
/// ever shown. For layouts saved before the panel existed: without this it
/// would open floating in the middle of the screen.
void dockBeside(const char* existing) {
    if (const ImGuiWindow* window = ImGui::FindWindowByName(existing); window != nullptr && window->DockId != 0) {
        ImGui::SetNextWindowDockID(window->DockId, ImGuiCond_FirstUseEver);
    }
}

} // namespace

// --- session -----------------------------------------------------------------

bool enterEditSession(EditorState& state, Renderer& renderer) {
    Scene& scene = renderer.scene();
    if (state.edit.has_value() || state.selected == kInvalidNode || !scene.isAlive(state.selected)) {
        return false;
    }
    Node& node = scene.node(state.selected);
    if (!renderer.resources().has(node.mesh)) {
        return false;
    }

    // One step of scene history for the whole session: undoing it after the
    // session ends puts the node's old mesh back, which is still registered.
    state.history.record(scene, state.selected);

    EditSession session;
    session.node = state.selected;
    session.originalMesh = node.mesh;
    session.geometry = renderer.resources().mesh(node.mesh).toEditable();
    session.mesh = renderer.createMesh(session.geometry, editedMesh(node.name));
    node.mesh = session.mesh;
    session.levelCamera = renderer.camera();
    session.levelGizmoMode = state.gizmoMode;
    state.gizmoMode = GizmoMode::Select;

    // Frame the object, looking at it the way the camera already looked. Room
    // to spare round it: the shape is about to grow.
    frameVertices(session, renderer, {}, 1.8f);
    placeOrbitCamera(session, renderer.camera());

    renderer.setIsolated(state.selected);
    renderer.setGridVisible(true);
    renderer.setSelected(kInvalidNode);
    renderer.setHighlighted(kInvalidNode);

    FUMAR_INFO("modelling '{}': {} vertices, {} faces", node.name, session.geometry.vertexCount(),
               session.geometry.faceCount());
    state.edit = std::move(session);
    return true;
}

void exitEditSession(EditorState& state, Renderer& renderer) {
    if (!state.edit.has_value()) {
        return;
    }
    flushEditSession(state, renderer);

    renderer.camera() = state.edit->levelCamera;
    state.gizmoMode = state.edit->levelGizmoMode;
    renderer.setIsolated(kInvalidNode);
    renderer.setGridVisible(false);

    FUMAR_INFO("finished modelling: {} vertices, {} faces", state.edit->geometry.vertexCount(),
               state.edit->geometry.faceCount());
    state.edit.reset();
}

void flushEditSession(EditorState& state, Renderer& renderer) {
    if (!state.edit.has_value() || !state.edit->dirty) {
        return;
    }
    renderer.updateMesh(state.edit->mesh, state.edit->geometry);
    state.edit->dirty = false;
}

void undoEdit(EditorState& state) {
    if (!state.edit.has_value() || state.edit->undo.empty()) {
        return;
    }
    EditSession& session = *state.edit;
    session.redo.push_back(EditSnapshot{session.geometry, session.selection, session.mode});
    EditSnapshot& previous = session.undo.back();
    session.geometry = std::move(previous.mesh);
    session.selection = std::move(previous.selection);
    session.mode = previous.mode;
    session.undo.pop_back();
    markChanged(session);
}

void redoEdit(EditorState& state) {
    if (!state.edit.has_value() || state.edit->redo.empty()) {
        return;
    }
    EditSession& session = *state.edit;
    session.undo.push_back(EditSnapshot{session.geometry, session.selection, session.mode});
    EditSnapshot& next = session.redo.back();
    session.geometry = std::move(next.mesh);
    session.selection = std::move(next.selection);
    session.mode = next.mode;
    session.redo.pop_back();
    markChanged(session);
}

// --- input -------------------------------------------------------------------

bool handleModelerShortcuts(EditorState& state, Renderer& renderer, bool navigating) {
    if (state.workspace != Workspace::Modeler || ImGui::GetIO().WantTextInput) {
        return state.edit.has_value();
    }

    if (!navigating && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
        if (state.edit.has_value()) {
            exitEditSession(state, renderer);
        } else if (!enterEditSession(state, renderer) && state.selected != kInvalidNode) {
            FUMAR_INFO("nothing to model: the selected node has no mesh");
        }
        return true;
    }

    if (!state.edit.has_value()) {
        return false;
    }
    if (navigating) {
        return true;
    }

    EditSession& session = *state.edit;
    const ImGuiIO& io = ImGui::GetIO();
    const auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, false); };

    if (pressed(ImGuiKey_1)) {
        setMode(session, geo::SelectMode::Vertex);
    }
    if (pressed(ImGuiKey_2)) {
        setMode(session, geo::SelectMode::Edge);
    }
    if (pressed(ImGuiKey_3)) {
        setMode(session, geo::SelectMode::Face);
    }

    if (io.KeyCtrl && pressed(ImGuiKey_Z)) {
        io.KeyShift ? redoEdit(state) : undoEdit(state);
    }
    if (io.KeyCtrl && pressed(ImGuiKey_Y)) {
        redoEdit(state);
    }
    if (io.KeyCtrl && pressed(ImGuiKey_R)) {
        perform(state, Operation::LoopCut);
    }

    if (io.KeyAlt) {
        if (pressed(ImGuiKey_E)) {
            perform(state, Operation::Extrude);
        }
        if (pressed(ImGuiKey_I)) {
            perform(state, Operation::Inset);
        }
        if (pressed(ImGuiKey_M)) {
            perform(state, Operation::Merge);
        }
        if (pressed(ImGuiKey_F)) {
            perform(state, Operation::Fill);
        }
        if (pressed(ImGuiKey_Z)) {
            session.xray = !session.xray;
        }
    } else if (!io.KeyCtrl) {
        if (pressed(ImGuiKey_A)) {
            session.selection =
                session.selection.empty() ? geo::selectAll(session.geometry, session.mode) : geo::Selection{};
        }
        if (pressed(ImGuiKey_X) || pressed(ImGuiKey_Delete)) {
            perform(state, Operation::Delete);
        }
        if (pressed(ImGuiKey_Escape)) {
            session.selection.clear();
        }
        // F frames what is selected, or the whole mesh when nothing is - the
        // way out after orbiting somewhere the object can no longer be found.
        if (pressed(ImGuiKey_F)) {
            frameVertices(session, renderer, geo::coveredVertices(session.geometry, session.selection, session.mode),
                          session.selection.empty() ? 1.8f : 2.5f);
            placeOrbitCamera(session, renderer.camera());
        }
    }

    return true;
}

void orbitEditCamera(EditorState& state, Renderer& renderer, Vec2 mouseDelta, bool pan) {
    if (!state.edit.has_value()) {
        return;
    }
    EditSession& session = *state.edit;
    Camera& camera = renderer.camera();

    if (pan) {
        // Scaled so a point at the target's depth stays under the cursor: the
        // view is 2 d tan(fov/2) tall there, spread over the viewport's height.
        const f32 viewHeight = static_cast<f32>(std::max(state.viewportSize.height, 1u));
        const f32 metresPerPixel =
            2.0f * session.orbitDistance * std::tan(radians(camera.fovYDegrees) * 0.5f) / viewHeight;
        session.orbitTarget -= camera.right() * (mouseDelta.x * metresPerPixel);
        session.orbitTarget += camera.up() * (mouseDelta.y * metresPerPixel);
    } else {
        // The fly camera's own look, then the camera put back on the sphere
        // round the target: turning to the right carries it off to the left,
        // so the object turns the way the mouse went.
        camera.yaw += mouseDelta.x * camera.lookSensitivity * 2.0f;
        camera.pitch = clamp(camera.pitch - mouseDelta.y * camera.lookSensitivity * 2.0f, -89.0f, 89.0f);
    }
    placeOrbitCamera(session, camera);
}

void zoomEditCamera(EditorState& state, Renderer& renderer, f32 wheel) {
    if (!state.edit.has_value() || wheel == 0.0f) {
        return;
    }
    EditSession& session = *state.edit;
    // Stops short of the target, which would leave nothing to look along, and
    // of the far plane, beyond which the object would vanish.
    session.orbitDistance =
        clamp(session.orbitDistance * std::pow(0.85f, wheel), 0.05f, renderer.camera().farPlane * 0.5f);
    placeOrbitCamera(session, renderer.camera());
}

bool drawEditViewport(EditorState& state, Renderer& renderer, Vec2 origin, Vec2 size, bool imageHovered) {
    if (!state.edit.has_value() || !renderer.scene().isAlive(state.edit->node)) {
        return false;
    }
    EditSession& session = *state.edit;
    const Projector projector = makeProjector(session, renderer, origin, size);

    refreshVisibility(session, renderer, projector);

    const ImVec2 mouseIm = ImGui::GetIO().MousePos;
    const Vec2 mouse{mouseIm.x, mouseIm.y};

    const bool gizmoHot = manipulateSelection(state, renderer, projector);
    updateHover(session, renderer, projector, mouse, imageHovered && !gizmoHot && !session.boxSelecting);
    drawOverlay(session, projector);

    // --- clicking and box selection ------------------------------------------
    // A press that turns into a drag of more than a few pixels is a box; one
    // that does not is a click. The decision is made at release, so the box
    // never flickers into existence for a click that wobbled slightly.
    if (imageHovered && !gizmoHot && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        session.pressed = true;
        session.boxStart = mouse;
    }
    if (session.pressed && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
        session.boxSelecting = true;
    }
    if (session.boxSelecting) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(imVec(session.boxStart), mouseIm, accent(0.12f));
        draw->AddRect(imVec(session.boxStart), mouseIm, accentBright(0.8f));
    }
    if (session.pressed && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const bool additive = ImGui::GetIO().KeyShift;
        if (session.boxSelecting) {
            boxSelect(session, projector, session.boxStart, mouse, additive);
        } else {
            clickSelect(session, additive);
        }
        session.pressed = false;
        session.boxSelecting = false;
    }

    if (session.statusSeconds > 0.0f) {
        session.statusSeconds -= ImGui::GetIO().DeltaTime;
    }

    return gizmoHot || session.boxSelecting;
}

// --- panels ------------------------------------------------------------------

void drawModelingToolsPanel(EditorState& state, Renderer& renderer) {
    dockBeside("Scripts");
    if (!ImGui::Begin("Modeling Tools")) {
        ImGui::End();
        return;
    }

    const f32 width = ImGui::GetContentRegionAvail().x;

    if (!state.edit.has_value()) {
        ImGui::SeparatorText("Modeler");
        ImGui::TextWrapped("Select an object, then press Tab to model it. The scene steps aside and "
                           "the object is left on its own; Tab again brings everything back.");
        ImGui::Spacing();

        const Scene& scene = renderer.scene();
        const bool canModel = state.selected != kInvalidNode && scene.isAlive(state.selected) &&
                              renderer.resources().has(scene.node(state.selected).mesh);
        ImGui::BeginDisabled(!canModel);
        const std::string label = canModel ? "Model '" + scene.node(state.selected).name + "'  (Tab)"
                                           : std::string("Model  (Tab)");
        if (ImGui::Button(label.c_str(), ImVec2(width, 0.0f))) {
            enterEditSession(state, renderer);
        }
        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::TextDisabled("Only that object changes: it gets a copy");
        ImGui::TextDisabled("of its mesh. To use the new shape on");
        ImGui::TextDisabled("another object, pick it under Mesh in");
        ImGui::TextDisabled("that object's Details.");
        ImGui::End();
        return;
    }

    EditSession& session = *state.edit;
    const geo::EditableMesh& mesh = session.geometry;

    ImGui::SeparatorText("Editing");
    ImGui::Text("%s", renderer.scene().isAlive(session.node) ? renderer.scene().node(session.node).name.c_str() : "?");
    if (ImGui::Button("Done  (Tab)", ImVec2(width, 0.0f))) {
        exitEditSession(state, renderer);
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Select");
    const f32 third = (width - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
    if (modeButton("Vertex", session.mode == geo::SelectMode::Vertex, "Vertices (1)", third)) {
        setMode(session, geo::SelectMode::Vertex);
    }
    ImGui::SameLine();
    if (modeButton("Edge", session.mode == geo::SelectMode::Edge, "Edges (2)", third)) {
        setMode(session, geo::SelectMode::Edge);
    }
    ImGui::SameLine();
    if (modeButton("Face", session.mode == geo::SelectMode::Face, "Faces (3)", third)) {
        setMode(session, geo::SelectMode::Face);
    }

    const usize selectedCount =
        session.selection.vertices.size() + session.selection.edges.size() + session.selection.faces.size();
    ImGui::Text("%zu vertices  %zu edges  %zu faces", mesh.vertexCount(), session.edgeVisible.size(),
                mesh.faceCount());
    ImGui::TextDisabled("%zu selected   A: all / none", selectedCount);

    ImGui::SeparatorText("Operations");
    const auto operationButton = [&](const char* label, Operation operation, const char* tooltip) {
        if (ImGui::Button(label, ImVec2(width, 0.0f))) {
            perform(state, operation);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tooltip);
        }
    };

    operationButton("Extrude   Alt+E", Operation::Extrude,
                    "Pull new geometry out of the selection. It stays selected,\n"
                    "with the Move gizmo on it: drag to give it depth.");
    operationButton("Inset   Alt+I", Operation::Inset,
                    "Shrink each selected face inside itself and ring it with quads.");
    ImGui::SetNextItemWidth(width * 0.5f);
    ImGui::DragFloat("thickness", &session.insetThickness, 0.005f, 0.001f, 10.0f, "%.3f m");
    operationButton("Loop cut   Ctrl+R", Operation::LoopCut,
                    "Split a ring of quads down the middle. Point at an edge across\n"
                    "the ring and press Ctrl+R; holding Ctrl shows which edge.");
    operationButton("Subdivide", Operation::Subdivide, "Cut each selected face into quads through its edge midpoints.");
    operationButton("Merge   Alt+M", Operation::Merge, "Collapse the selected vertices into one, at their middle.");
    operationButton("Fill   Alt+F", Operation::Fill,
                    "Make a face from two edges, a closed loop of edges, or\n"
                    "three or more vertices.");
    operationButton("Delete   X", Operation::Delete, "Remove the selection and whatever depended on it.");
    operationButton("Flip normals", Operation::FlipNormals, "Turn the selected faces inside out.");

    ImGui::SeparatorText("Display");
    ImGui::Checkbox("X-ray   Alt+Z", &session.xray);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Show and select what is on the far side of the mesh too.");
    }

    ImGui::SetNextItemWidth(width * 0.5f);
    if (ImGui::SliderFloat("smooth angle", &session.geometry.smoothAngleDegrees, 0.0f, 90.0f, "%.0f deg")) {
        markChanged(session);
    }
    if (ImGui::IsItemActivated()) {
        pushUndo(session);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Faces meeting at less than this angle are shaded as one\n"
                          "smooth surface. 0 shades every face flat.");
    }

    if (session.statusSeconds > 0.0f && !session.status.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(uiColor(1.0f, 0.72f, 0.45f), "%s", session.status.c_str());
    }

    ImGui::End();
}

void drawMaterialsPanel(EditorState& state, Renderer& renderer) {
    dockBeside("Content");
    if (!ImGui::Begin("Materials")) {
        ImGui::End();
        return;
    }

    Scene& scene = renderer.scene();
    ResourceRegistry& resources = renderer.resources();
    const bool hasTarget = state.selected != kInvalidNode && scene.isAlive(state.selected) &&
                           resources.has(scene.node(state.selected).mesh);
    const MaterialHandle current = hasTarget ? scene.node(state.selected).material : MaterialHandle{};

    if (!hasTarget) {
        ImGui::TextDisabled("Select an object to give it a material.");
    }

    // A grid of swatches with names, wrapping to the panel width. Clicking one
    // puts it on the selected object.
    const f32 cell = 120.0f;
    const i32 columns = std::max(1, static_cast<i32>(ImGui::GetContentRegionAvail().x / cell));
    if (ImGui::BeginTable("##materials", columns)) {
        for (u32 i = 0; i < static_cast<u32>(resources.materialCount()); ++i) {
            const MaterialHandle handle{i};
            const Material& material = resources.material(handle);
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));

            const ImVec4 swatch(material.baseColorFactor.x, material.baseColorFactor.y, material.baseColorFactor.z, 1.0f);
            ImGui::BeginDisabled(!hasTarget);
            if (ImGui::ColorButton("##swatch", swatch, ImGuiColorEditFlags_NoTooltip, ImVec2(cell - 16.0f, 40.0f))) {
                state.history.record(scene, state.selected);
                scene.node(state.selected).material = handle;
            }
            ImGui::EndDisabled();
            if (current == handle) {
                ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), accentBright(), 3.0f,
                                                    0, 2.0f);
            }
            ImGui::TextUnformatted(material.name.empty() ? "(unnamed)" : material.name.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

// --- smoke test ----------------------------------------------------------------

bool runModelerSmoke(EditorState& state, Renderer& renderer, u32 frame, const std::string& variant) {
    Scene& scene = renderer.scene();
    const bool finish = variant == "finish";

    if (variant == "helmet") {
        using Clock = std::chrono::steady_clock;
        if (frame == 20) {
            state.workspace = Workspace::Modeler;
            scene.traverse([&](NodeId id, u32) {
                if (scene.node(id).name.starts_with("node_damagedHelmet")) {
                    state.selected = id;
                }
            });
            const auto start = Clock::now();
            const bool entered = enterEditSession(state, renderer);
            const f64 ms = std::chrono::duration<f64, std::milli>(Clock::now() - start).count();
            FUMAR_INFO("modeler smoke: helmet {} in {:.1f} ms", entered ? "opened" : "FAILED to open", ms);
        }
        if (frame == 21 && state.edit.has_value()) {
            // What refreshVisibility does when the camera moves: a ray per
            // vertex against the whole mesh.
            const auto start = Clock::now();
            const geo::RayCaster caster(state.edit->geometry);
            const f64 buildMs = std::chrono::duration<f64, std::milli>(Clock::now() - start).count();
            const Vec3 eye = xyz(inverse(scene.worldTransform(state.edit->node)) * point(renderer.camera().position));
            usize hidden = 0;
            const auto traceStart = Clock::now();
            for (const Vec3 p : state.edit->geometry.positions) {
                hidden += caster.occluded(eye, p - eye, 0.995f) ? 1 : 0;
            }
            const f64 traceMs = std::chrono::duration<f64, std::milli>(Clock::now() - traceStart).count();
            FUMAR_INFO("modeler smoke: helmet {} vertices, {} faces, {}; tree built in {:.1f} ms, "
                       "{} vertices tested in {:.1f} ms, {} hidden",
                       state.edit->geometry.vertexCount(), state.edit->geometry.faceCount(),
                       geo::validate(state.edit->geometry).empty() ? "sound" : "NOT sound", buildMs,
                       state.edit->geometry.vertexCount(), traceMs, hidden);
            return false;
        }
        return true;
    }

    const auto report = [&](const char* step) {
        if (!state.edit.has_value()) {
            FUMAR_ERROR("modeler smoke: {} - no session open", step);
            return;
        }
        const auto problems = geo::validate(state.edit->geometry);
        FUMAR_INFO("modeler smoke: {} -> {} vertices, {} faces, {}", step, state.edit->geometry.vertexCount(),
                   state.edit->geometry.faceCount(), problems.empty() ? "sound" : problems.front());
    };

    switch (frame) {
    case 20: {
        state.workspace = Workspace::Modeler;
        NodeId block = kInvalidNode;
        scene.traverse([&](NodeId id, u32) {
            if (scene.node(id).name == "Block A") {
                block = id;
            }
        });
        state.selected = block;
        FUMAR_INFO("modeler smoke: Modeler workspace, Block A selected ({})", block != kInvalidNode ? "found" : "MISSING");
        break;
    }
    case 30:
        FUMAR_INFO("modeler smoke: entering: {}", enterEditSession(state, renderer) ? "ok" : "FAILED");
        break;
    case 40: {
        if (!state.edit.has_value()) {
            break;
        }
        EditSession& session = *state.edit;
        session.mode = geo::SelectMode::Face;
        session.selection.clear();
        const Mat4& world = scene.worldTransform(session.node);
        for (u32 f = 0; f < session.geometry.faces.size(); ++f) {
            const Vec3 normal = normalize(xyz(world * direction(geo::faceNormal(session.geometry, session.geometry.faces[f]))));
            if (normal.y > 0.9f) {
                session.selection.faces.insert(f);
            }
        }
        perform(state, Operation::Extrude);
        report("extrude top");
        break;
    }
    case 50: {
        if (!state.edit.has_value()) {
            break;
        }
        // What a drag of the Move gizmo straight up would do.
        EditSession& session = *state.edit;
        pushUndo(session);
        const Vec3 up = xyz(inverse(scene.worldTransform(session.node)) * direction(Vec3{0.0f, 0.6f, 0.0f}));
        for (const u32 v : geo::coveredVertices(session.geometry, session.selection, session.mode)) {
            session.geometry.positions[v] += up;
        }
        markChanged(session);
        report("lift");
        break;
    }
    case 60:
        perform(state, Operation::Inset);
        report("inset");
        break;
    case 70: {
        if (!state.edit.has_value()) {
            break;
        }
        // A vertical edge of the original sides: both ends below the lifted
        // part, one at the bottom of the block and one at the top.
        EditSession& session = *state.edit;
        for (const geo::Edge& edge : session.geometry.buildEdges().edges) {
            const Vec3 a = session.geometry.positions[edge.key.a];
            const Vec3 b = session.geometry.positions[edge.key.b];
            if (std::abs(a.x - b.x) < 1e-4f && std::abs(a.z - b.z) < 1e-4f && std::abs(a.y - b.y) > 0.9f &&
                std::min(a.y, b.y) < -0.4f) {
                session.loopEdge = edge.key;
                session.loopEdgeValid = true;
                break;
            }
        }
        session.mode = geo::SelectMode::Face;
        perform(state, Operation::LoopCut);
        report("loop cut");
        break;
    }
    case 75: {
        if (!state.edit.has_value()) {
            break;
        }
        // What dragging with the middle button, then with Shift, then a few
        // wheel notches would do. Whatever the moves, the camera has to stay
        // on its sphere and keep looking straight at the target.
        const EditSession& session = *state.edit;
        const auto check = [&](const char* step) {
            const Camera& camera = renderer.camera();
            const Vec3 toTarget = session.orbitTarget - camera.position;
            FUMAR_INFO("modeler smoke: {} -> yaw {:.1f}, pitch {:.1f}, distance {:.3f} (orbit {:.3f}), aim {:.5f}", step,
                       camera.yaw, camera.pitch, length(toTarget), session.orbitDistance,
                       dot(normalize(toTarget), camera.forward()));
        };
        check("framed");
        FUMAR_INFO("modeler smoke: camera at ({:.2f}, {:.2f}, {:.2f}), target ({:.2f}, {:.2f}, {:.2f})",
                   renderer.camera().position.x, renderer.camera().position.y, renderer.camera().position.z,
                   session.orbitTarget.x, session.orbitTarget.y, session.orbitTarget.z);
        if (variant != "orbit") {
            break;
        }
        orbitEditCamera(state, renderer, Vec2{250.0f, -120.0f}, false);
        check("orbited");
        const Vec3 before = session.orbitTarget;
        orbitEditCamera(state, renderer, Vec2{-80.0f, 40.0f}, true);
        FUMAR_INFO("modeler smoke: panned the target by {:.3f} m", length(session.orbitTarget - before));
        check("panned");
        zoomEditCamera(state, renderer, 3.0f);
        check("zoomed in");
        break;
    }
    case 80:
        if (variant != "save") {
            break;
        }
        exitEditSession(state, renderer);
        state.smokeScene = (executableDirectory() / "scenes" / "modeler_smoke.fumar").string();
        FUMAR_INFO("modeler smoke: saved: {}", saveScene(renderer, state.smokeScene) ? "ok" : "FAILED");
        break;
    case 85:
        if (variant != "save") {
            break;
        }
        state.selected = kInvalidNode;
        state.history.clear();
        FUMAR_INFO("modeler smoke: reloaded: {}", loadScene(renderer, state.smokeScene) ? "ok" : "FAILED");
        break;
    case 88:
        if (variant != "save") {
            break;
        }
        scene.traverse([&](NodeId id, u32) {
            if (scene.node(id).name == "Block A") {
                state.selected = id;
            }
        });
        state.workspace = Workspace::Modeler;
        enterEditSession(state, renderer);
        if (state.edit.has_value()) {
            report("after save and reload");
            // Read from the file's extras, not reconstructed: planar quads
            // would come back as quads either way, so the face count alone
            // would not show whether the polygons were actually saved.
            const usize sizes = renderer.resources().mesh(state.edit->originalMesh).polygonSizes().size();
            FUMAR_INFO("modeler smoke: the file carried {} polygon sizes for {} faces", sizes,
                       state.edit->geometry.faceCount());
        }
        return false;
    case 90:
        if (!finish) {
            return false;
        }
        exitEditSession(state, renderer);
        FUMAR_INFO("modeler smoke: closed, isolation {}", renderer.isolated() == kInvalidNode ? "off" : "STILL ON");
        break;
    case 110: {
        NodeId restored = kInvalidNode;
        const NodeId block = state.selected;
        const MeshHandle edited = scene.isAlive(block) ? scene.node(block).mesh : MeshHandle{};
        state.history.undo(scene, state.selected, restored);
        state.selected = restored;
        const MeshHandle now = scene.isAlive(restored) ? scene.node(restored).mesh : MeshHandle{};
        FUMAR_INFO("modeler smoke: scene undo {} the mesh ({} -> {})", now != edited ? "restored" : "DID NOT restore",
                   edited.index, now.index);
        return false;
    }
    default:
        break;
    }
    return true;
}

} // namespace fumar
