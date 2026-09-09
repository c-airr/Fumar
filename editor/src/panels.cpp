#include "panels.hpp"

#include "fumar/core/math.hpp"
#include "fumar/render/renderer.hpp"

#include <ImGuizmo.h>
#include <imgui.h>
// DockBuilder is not part of the public API - it lives in imgui_internal.h and
// is the only way to arrange panels from code rather than by dragging them.
#include <imgui_internal.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace fumar {
namespace {

constexpr ImVec4 kAccent{0.659f, 0.373f, 0.173f, 1.00f};

/// Degrees from a quaternion, for display and editing.
///
/// Euler angles are a poor way to STORE a rotation - they gimbal lock, and the
/// same orientation has several representations. They are the only sane way to
/// show one, though: nobody edits a quaternion by hand. So the conversion
/// happens here, at the boundary, and the scene keeps the quaternion.
Vec3 toEulerDegrees(const Quat& q) {
    // The clamp guards the asin: rounding can push the argument just past 1 and
    // produce NaN.
    const f32 sinPitch = clamp(2.0f * (q.w * q.x - q.y * q.z), -1.0f, 1.0f);

    const f32 pitch = std::asin(sinPitch);
    const f32 yaw = std::atan2(2.0f * (q.w * q.y + q.z * q.x), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const f32 roll = std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.x * q.x + q.z * q.z));

    return Vec3{degrees(pitch), degrees(yaw), degrees(roll)};
}

Quat fromEulerDegrees(const Vec3& euler) {
    // Applied in yaw, then pitch, then roll order, matching the extraction.
    const Quat yaw = fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, radians(euler.y));
    const Quat pitch = fromAxisAngle(Vec3{1.0f, 0.0f, 0.0f}, radians(euler.x));
    const Quat roll = fromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, radians(euler.z));
    return normalize(yaw * pitch * roll);
}

/// A toolbar button that shows its active state through the accent colour.
bool toolButton(const char* label, bool active, const char* tooltip) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.72f, 0.45f, 1.0f));
    }

    const bool pressed = ImGui::Button(label);

    if (active) {
        ImGui::PopStyleColor(2);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return pressed;
}

/// Arranges the panels the first time the editor runs.
///
/// Without this every panel opens at the same default position, stacked on top
/// of each other, which looks broken. After the first run the arrangement is
/// restored from the .ini file next to the executable.
void buildDefaultLayout(ImGuiID dockspaceId) {
    ImGui::DockBuilderRemoveNode(dockspaceId);

    // The two flag sets come from different enums, so combining them directly
    // is a deprecated implicit conversion. Going through int is explicit.
    const auto nodeFlags = static_cast<ImGuiDockNodeFlags>(
        static_cast<int>(ImGuiDockNodeFlags_DockSpace) |
        static_cast<int>(ImGuiDockNodeFlags_PassthruCentralNode));
    ImGui::DockBuilderAddNode(dockspaceId, nodeFlags);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    // Each split carves a strip off what remains, so order matters. The last
    // argument matters as much as the return value: splitting turns the node
    // into a PARENT of the two halves, and only leaf nodes hold windows, so the
    // id of the remaining half has to be written back through it. Passing
    // nullptr and reusing the original id docks against a parent, which
    // silently leaves the window floating.
    ImGuiID remainder = dockspaceId;
    ImGuiID leftTop = ImGui::DockBuilderSplitNode(remainder, ImGuiDir_Left, 0.26f, nullptr, &remainder);
    const ImGuiID leftBottom =
        ImGui::DockBuilderSplitNode(leftTop, ImGuiDir_Down, 0.45f, nullptr, &leftTop);

    // The viewport takes the top of the right-hand area, details the bottom.
    ImGuiID viewport = remainder;
    const ImGuiID details = ImGui::DockBuilderSplitNode(viewport, ImGuiDir_Down, 0.32f, nullptr, &viewport);

    ImGui::DockBuilderDockWindow("World Outliner", leftTop);
    ImGui::DockBuilderDockWindow("Content", leftBottom);
    ImGui::DockBuilderDockWindow("Statistics", leftBottom);
    ImGui::DockBuilderDockWindow("Viewport", viewport);
    ImGui::DockBuilderDockWindow("Details", details);

    ImGui::DockBuilderFinish(dockspaceId);
}

/// One node and its subtree in the outliner.
void drawNodeRecursive(EditorState& state, Scene& scene, NodeId id) {
    // Captured up front: the popup below can create a node, which may
    // reallocate the scene storage and invalidate any reference held here.
    const bool isLeaf = scene.node(id).children.empty();
    const bool hidden = !scene.node(id).visible;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen;
    if (isLeaf) {
        // NoTreePushOnOpen means TreeNodeEx returns true WITHOUT pushing onto
        // the id stack, so a matching TreePop must not be called for this node.
        // Calling one anyway trips the IDStack assertion inside ImGui.
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (id == state.selected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    const bool tint = hidden || id == state.hovered;
    if (hidden) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.45f, 0.47f, 1.0f));
    } else if (id == state.hovered) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.45f, 1.0f));
    }

    // The pointer-shaped id is what keeps ImGui per-row state attached to the
    // right node when siblings are added or removed.
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<usize>(id)), flags, "%s",
                                        scene.node(id).name.c_str());

    if (tint) {
        ImGui::PopStyleColor();
    }

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        state.selected = id;
    }

    // Both actions are deferred: mutating the scene mid-row would leave ImGui
    // reading a freed name and would change the vector being walked below.
    bool destroyRequested = false;
    bool addChildRequested = false;
    bool duplicateRequested = false;

    if (ImGui::BeginPopupContextItem()) {
        state.selected = id;
        if (ImGui::MenuItem(hidden ? "Show" : "Hide")) {
            // Visibility is inherited, so this hides the whole subtree.
            scene.node(id).visible = hidden;
        }
        if (ImGui::MenuItem("Add child")) {
            addChildRequested = true;
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
            duplicateRequested = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete")) {
            destroyRequested = true;
        }
        ImGui::EndPopup();
    }

    if (open && !isLeaf) {
        // Copied, because deleting a child would otherwise mutate the vector
        // being iterated.
        const std::vector<NodeId> children = scene.node(id).children;
        for (NodeId child : children) {
            if (scene.isAlive(child)) {
                drawNodeRecursive(state, scene, child);
            }
        }
        ImGui::TreePop();
    }

    if (addChildRequested) {
        state.selected = scene.createNode("node", id);
    }
    if (duplicateRequested) {
        const NodeId copy = scene.duplicateNode(id);
        if (copy != kInvalidNode) {
            state.selected = copy;
        }
    }
    if (destroyRequested) {
        if (state.selected == id) {
            state.selected = kInvalidNode;
        }
        scene.destroyNode(id);
    }
}

} // namespace

void drawDockspace(EditorState& state) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##dockhost", nullptr, flags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspaceId = ImGui::GetID("fumar_dockspace");

    // A null node means nothing was restored from the .ini, so this is a first
    // run and the panels need somewhere to go.
    if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr || state.resetLayoutRequested) {
        state.resetLayoutRequested = false;
        buildDefaultLayout(dockspaceId);
    }

    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Window")) {
            ImGui::MenuItem("World Outliner", nullptr, &state.showOutliner);
            ImGui::MenuItem("Details", nullptr, &state.showDetails);
            ImGui::MenuItem("Content", nullptr, &state.showContent);
            ImGui::MenuItem("Statistics", nullptr, &state.showStats);
            ImGui::Separator();
            if (ImGui::MenuItem("Reset layout")) {
                state.resetLayoutRequested = true;
            }
            ImGui::MenuItem("ImGui demo", nullptr, &state.showImGuiDemo);
            ImGui::EndMenu();
        }

        const char* hint =
            "right mouse: look   |   WASD: move   |   Q W E R: tools   |   Ctrl+D: duplicate   |   Del: delete";
        const f32 hintWidth = ImGui::CalcTextSize(hint).x;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - hintWidth - ImGui::GetStyle().WindowPadding.x * 2.0f);
        ImGui::TextDisabled("%s", hint);

        ImGui::EndMenuBar();
    }

    ImGui::End();
}

void drawViewportPanel(EditorState& state, Renderer& renderer) {
    // No padding: the scene image should meet the panel edge, the way a
    // viewport does in every editor.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    // --- toolbar ------------------------------------------------------------
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
    ImGui::BeginChild("##viewport_toolbar", ImVec2(0.0f, ImGui::GetFrameHeight() + 12.0f),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);

    if (toolButton("Select", state.gizmoMode == GizmoMode::Select, "Select (Q)")) {
        state.gizmoMode = GizmoMode::Select;
    }
    ImGui::SameLine();
    if (toolButton("Move", state.gizmoMode == GizmoMode::Translate, "Move (W)")) {
        state.gizmoMode = GizmoMode::Translate;
    }
    ImGui::SameLine();
    if (toolButton("Rotate", state.gizmoMode == GizmoMode::Rotate, "Rotate (E)")) {
        state.gizmoMode = GizmoMode::Rotate;
    }
    ImGui::SameLine();
    if (toolButton("Scale", state.gizmoMode == GizmoMode::Scale, "Scale (R)")) {
        state.gizmoMode = GizmoMode::Scale;
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    if (toolButton(state.gizmoLocalSpace ? "Local" : "World", false, "Axes the gizmo works along")) {
        state.gizmoLocalSpace = !state.gizmoLocalSpace;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &state.snapEnabled);

    if (state.gizmoMode == GizmoMode::Scale) {
        ImGui::SameLine();
        ImGui::TextDisabled(ImGui::GetIO().KeyShift ? "| uniform" : "| shift: uniform");
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();

    // --- scene image --------------------------------------------------------
    const ImVec2 available = ImGui::GetContentRegionAvail();
    state.viewportSize = Extent2D{static_cast<u32>(available.x > 1.0f ? available.x : 1.0f),
                                  static_cast<u32>(available.y > 1.0f ? available.y : 1.0f)};

    const ImVec2 imageOrigin = ImGui::GetCursorScreenPos();

    if (state.viewportTexture != 0) {
        ImGui::Image(state.viewportTexture, available);
    } else {
        ImGui::Dummy(available);
    }

    const bool imageHovered = ImGui::IsItemHovered();

    // Cursor position inside the image, normalised. Picking and the gizmo both
    // need it in the image own coordinates, not the window ones.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    state.viewportCursor = Vec2{
        available.x > 0.0f ? (mouse.x - imageOrigin.x) / available.x : 0.0f,
        available.y > 0.0f ? (mouse.y - imageOrigin.y) / available.y : 0.0f,
    };

    // --- gizmo --------------------------------------------------------------
    bool gizmoActive = false;

    Scene& scene = renderer.scene();
    if (state.gizmoMode != GizmoMode::Select && state.selected != kInvalidNode &&
        scene.isAlive(state.selected)) {

        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(imageOrigin.x, imageOrigin.y, available.x, available.y);

        const f32 aspect = available.y > 0.0f ? available.x / available.y : 1.0f;
        const Mat4 view = renderer.camera().view();
        Mat4 projection = renderer.camera().projection(aspect);

        // ImGuizmo assumes an OpenGL-style projection with Y up. fumar negates
        // Y to match Vulkan, so it has to be negated back for the gizmo alone -
        // otherwise dragging up moves the object down.
        projection.at(1, 1) = -projection.at(1, 1);

        ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
        f32 snapValue = state.translateSnap;
        if (state.gizmoMode == GizmoMode::Rotate) {
            operation = ImGuizmo::ROTATE;
            snapValue = state.rotateSnap;
        } else if (state.gizmoMode == GizmoMode::Scale) {
            // Holding shift switches to uniform scaling: one handle drives all
            // three axes together, which is what you want whenever the object
            // should keep its proportions.
            operation = ImGui::GetIO().KeyShift ? ImGuizmo::SCALEU : ImGuizmo::SCALE;
            snapValue = state.scaleSnap;
        }

        // Scale is always local: scaling along a world axis on a rotated object
        // would shear it, which a position/rotation/scale transform cannot
        // represent.
        const ImGuizmo::MODE mode =
            (state.gizmoLocalSpace || operation == ImGuizmo::SCALE) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

        // The gizmo works in world space, but a node stores its transform
        // relative to its parent - so the result has to be brought back through
        // the inverse of the parent world transform.
        const NodeId parent = scene.node(state.selected).parent;
        const Mat4 parentWorld = scene.worldTransform(parent);
        Mat4 world = scene.worldTransform(state.selected);

        const f32 snapVector[3]{snapValue, snapValue, snapValue};

        if (ImGuizmo::Manipulate(&view.columns[0].x, &projection.columns[0].x, operation, mode,
                                 &world.columns[0].x, nullptr,
                                 state.snapEnabled ? snapVector : nullptr)) {
            const Mat4 local = inverse(parentWorld) * world;
            scene.node(state.selected).transform = Transform::fromMatrix(local);
        }

        gizmoActive = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
    }

    // Hovering the gizmo must not count as hovering the scene, or clicking a
    // handle would also pick whatever is behind it.
    state.viewportHovered = imageHovered && !gizmoActive;

    ImGui::End();
}

void drawOutlinerPanel(EditorState& state, Scene& scene) {
    if (!state.showOutliner) {
        return;
    }

    if (ImGui::Begin("World Outliner", &state.showOutliner)) {
        if (ImGui::Button("Add empty")) {
            state.selected = scene.createNode("node");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%zu nodes", scene.nodeCount());
        ImGui::Separator();

        // The root itself is not shown - it is an implementation detail, and a
        // permanent row that cannot be deleted would only be in the way.
        const std::vector<NodeId> topLevel = scene.node(kRootNode).children;
        for (NodeId child : topLevel) {
            if (scene.isAlive(child)) {
                drawNodeRecursive(state, scene, child);
            }
        }

        // Clicking empty space clears the selection, which is what every editor
        // does and what people expect.
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !ImGui::IsAnyItemHovered()) {
            state.selected = kInvalidNode;
        }
    }
    ImGui::End();
}

void drawDetailsPanel(EditorState& state, Scene& scene, const Renderer& renderer) {
    if (!state.showDetails) {
        return;
    }

    if (ImGui::Begin("Details", &state.showDetails)) {
        if (state.selected == kInvalidNode || !scene.isAlive(state.selected)) {
            ImGui::TextDisabled("Select an object to see its properties.");
            ImGui::End();
            return;
        }

        Node& node = scene.node(state.selected);

        // A fixed-size buffer rather than binding std::string directly: ImGui
        // text fields write into raw memory, and a string would have to be
        // resized from a callback.
        char nameBuffer[128];
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", node.name.c_str());
        if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer))) {
            node.name = nameBuffer;
        }

        ImGui::Checkbox("Visible", &node.visible);

        ImGui::SeparatorText("Transform");

        ImGui::DragFloat3("Location", &node.transform.position.x, 0.02f);

        // Rotation round-trips through Euler angles for editing. Written back
        // only when touched, so an untouched quaternion never loses precision
        // to the conversion.
        Vec3 euler = toEulerDegrees(node.transform.rotation);
        if (ImGui::DragFloat3("Rotation", &euler.x, 0.5f)) {
            node.transform.rotation = fromEulerDegrees(euler);
        }

        ImGui::DragFloat3("Scale", &node.transform.scale.x, 0.01f, 0.001f, 1000.0f);

        if (ImGui::Button("Reset transform")) {
            node.transform = Transform{};
        }

        ImGui::SeparatorText("Rendering");
        if (node.mesh.valid() && renderer.resources().has(node.mesh)) {
            ImGui::Text("Mesh: #%u (%u indices)", node.mesh.index,
                        renderer.resources().mesh(node.mesh).indexCount());
        } else {
            ImGui::TextDisabled("No mesh - this node only groups others.");
        }

        if (node.material.valid() && renderer.resources().has(node.material)) {
            const Material& material = renderer.resources().material(node.material);
            ImGui::Text("Material: %s", material.name.c_str());

            // Read-only preview of the colour: editing it here would change it
            // for every object sharing the material, which is a surprise best
            // left until materials are their own asset in the content panel.
            const ImVec4 colour(material.baseColorFactor.x, material.baseColorFactor.y,
                                material.baseColorFactor.z, material.baseColorFactor.w);
            ImGui::ColorButton("##material_colour", colour, ImGuiColorEditFlags_NoTooltip,
                               ImVec2(ImGui::GetContentRegionAvail().x, 18.0f));
        } else {
            ImGui::TextDisabled("Material: default");
        }

        ImGui::SeparatorText("Hierarchy");
        ImGui::Text("Parent: %s",
                    node.parent == kRootNode ? "(scene root)" : scene.node(node.parent).name.c_str());
        ImGui::Text("Children: %zu", node.children.size());
    }
    ImGui::End();
}

void drawContentPanel(EditorState& state, Renderer& renderer) {
    if (!state.showContent) {
        return;
    }

    if (ImGui::Begin("Content", &state.showContent)) {
        ImGui::SeparatorText("Place");

        Scene& scene = renderer.scene();

        // The handles are created once and reused: a new mesh per placed cube
        // would upload the same vertices to the GPU again every time.
        static MeshHandle cubeMesh;
        static MeshHandle cylinderMesh;
        static MeshHandle planeMesh;
        static MaterialHandle stoneMaterial;

        if (!renderer.resources().has(cubeMesh)) {
            cubeMesh = renderer.createCubeMesh();
            cylinderMesh = renderer.createCylinderMesh(0.5f, 2.0f);
            planeMesh = renderer.createPlaneMesh(1.0f);
            stoneMaterial = renderer.createMaterial("Stone", Vec4{0.55f, 0.55f, 0.58f, 1.0f});
        }

        // Spawned in front of the camera rather than at the origin, so a new
        // object appears where you are looking instead of somewhere off screen.
        const auto place = [&](const char* name, MeshHandle mesh, MaterialHandle material) {
            const NodeId id = scene.createNode(name);
            Node& node = scene.node(id);
            node.mesh = mesh;
            node.material = material;
            node.transform.position = renderer.camera().position + renderer.camera().forward() * 6.0f;
            state.selected = id;
        };

        if (ImGui::Button("Cube", ImVec2(-1.0f, 0.0f))) {
            place("Cube", cubeMesh, stoneMaterial);
        }
        if (ImGui::Button("Cylinder", ImVec2(-1.0f, 0.0f))) {
            place("Cylinder", cylinderMesh, stoneMaterial);
        }
        if (ImGui::Button("Plane", ImVec2(-1.0f, 0.0f))) {
            place("Plane", planeMesh, stoneMaterial);
        }

        ImGui::SeparatorText("Assets");
        ImGui::Text("Meshes: %zu", renderer.resources().meshCount());
        ImGui::Text("Materials: %zu", renderer.resources().materialCount());
        ImGui::Text("Textures: %zu", renderer.resources().textureCount());
    }
    ImGui::End();
}

void drawStatsPanel(EditorState& state, const Scene& scene, const Renderer& renderer) {
    if (!state.showStats) {
        return;
    }

    if (ImGui::Begin("Statistics", &state.showStats)) {
        // Exponential smoothing: the raw frame time flickers too fast to read,
        // and an average over a fixed window lags behind real changes.
        const f32 frameMs = 1000.0f / ImGui::GetIO().Framerate;
        state.smoothedFrameMs = state.smoothedFrameMs * 0.92f + frameMs * 0.08f;

        ImGui::Text("%.2f ms/frame (%.0f fps)", static_cast<f64>(state.smoothedFrameMs),
                    static_cast<f64>(1000.0f / state.smoothedFrameMs));

        ImGui::SeparatorText("Scene");
        ImGui::Text("Nodes: %zu", scene.nodeCount());

        usize drawables = 0;
        scene.forEachDrawable([&drawables](NodeId, const Node&, const Mat4&) { ++drawables; });
        ImGui::Text("Draw calls: %zu", drawables);

        ImGui::SeparatorText("Viewport");
        ImGui::Text("%ux%u px", renderer.viewportExtent().width, renderer.viewportExtent().height);

        ImGui::SeparatorText("Camera");
        const Camera& camera = renderer.camera();
        ImGui::Text("Location: %.2f, %.2f, %.2f", static_cast<f64>(camera.position.x),
                    static_cast<f64>(camera.position.y), static_cast<f64>(camera.position.z));
        ImGui::Text("Yaw / pitch: %.1f / %.1f", static_cast<f64>(camera.yaw),
                    static_cast<f64>(camera.pitch));
    }
    ImGui::End();
}

} // namespace fumar
