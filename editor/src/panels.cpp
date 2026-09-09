#include "panels.hpp"

#include "fumar/core/math.hpp"
#include "fumar/render/renderer.hpp"

#include <imgui.h>
// DockBuilder is not part of the public API - it lives in imgui_internal.h and
// is the only way to arrange panels from code rather than by dragging them.
#include <imgui_internal.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace fumar {
namespace {

/// Degrees from a quaternion, for display and editing.
///
/// Euler angles are a poor way to STORE a rotation - they gimbal lock, and the
/// same orientation has several representations. They are the only sane way to
/// show one, though: nobody edits a quaternion by hand. So the conversion
/// happens here, at the boundary, and the scene keeps the quaternion.
Vec3 toEulerDegrees(const Quat& q) {
    // Standard yaw-pitch-roll extraction. The clamp guards the asin: rounding
    // can push the argument just past 1 and produce NaN.
    const f32 sinPitch = clamp(2.0f * (q.w * q.x - q.y * q.z), -1.0f, 1.0f);

    const f32 pitch = std::asin(sinPitch);
    const f32 yaw = std::atan2(2.0f * (q.w * q.y + q.z * q.x), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const f32 roll = std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.x * q.x + q.z * q.z));

    return Vec3{degrees(pitch), degrees(yaw), degrees(roll)};
}

Quat fromEulerDegrees(const Vec3& euler) {
    // Applied in yaw, then pitch, then roll order, matching the extraction above.
    const Quat yaw = fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, radians(euler.y));
    const Quat pitch = fromAxisAngle(Vec3{1.0f, 0.0f, 0.0f}, radians(euler.x));
    const Quat roll = fromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, radians(euler.z));
    return normalize(yaw * pitch * roll);
}

/// Arranges the panels the first time the editor runs.
///
/// Without this every panel opens at the same default position, stacked on top
/// of each other, which looks broken. After the first run the arrangement is
/// restored from the .ini file next to the executable, and this is skipped.
void buildDefaultLayout(ImGuiID dockspaceId) {
    ImGui::DockBuilderRemoveNode(dockspaceId);

    // The two flag sets come from different enums, so combining them directly
    // is a deprecated implicit conversion. Going through int is explicit about
    // what is intended.
    const auto nodeFlags = static_cast<ImGuiDockNodeFlags>(
        static_cast<int>(ImGuiDockNodeFlags_DockSpace) |
        static_cast<int>(ImGuiDockNodeFlags_PassthruCentralNode));
    ImGui::DockBuilderAddNode(dockspaceId, nodeFlags);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    // Each split carves a strip off what remains, so order matters: the left
    // column comes out of the full width, the right column out of what is left,
    // and whatever survives stays as the viewport.
    //
    // The last argument matters as much as the return value. Splitting turns
    // the node being split into a PARENT of the two halves, and only leaf nodes
    // can hold windows - so the id of the remaining half has to be written back
    // through that out-parameter. Passing nullptr and reusing the original id
    // docks against a parent, which silently leaves the window floating.
    ImGuiID centre = dockspaceId;
    ImGuiID leftTop = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.20f, nullptr, &centre);
    const ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.22f, nullptr, &centre);
    const ImGuiID leftBottom = ImGui::DockBuilderSplitNode(leftTop, ImGuiDir_Down, 0.55f, nullptr, &leftTop);

    ImGui::DockBuilderDockWindow("Hierarchy", leftTop);
    ImGui::DockBuilderDockWindow("Inspector", leftBottom);
    ImGui::DockBuilderDockWindow("Statistics", right);

    ImGui::DockBuilderFinish(dockspaceId);
}

/// One node and its subtree in the hierarchy panel.
void drawNodeRecursive(EditorState& state, Scene& scene, NodeId id) {
    // Captured up front: the popup below can create a node, which may
    // reallocate the scene's storage and invalidate any reference held here.
    const bool isLeaf = scene.node(id).children.empty();
    const bool hidden = !scene.node(id).visible;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (isLeaf) {
        // NoTreePushOnOpen means TreeNodeEx returns true WITHOUT pushing onto
        // the id stack, so a matching TreePop must not be called for this node.
        // Calling one anyway is what trips the IDStack assertion inside ImGui.
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (id == state.selected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    // Hidden nodes are dimmed rather than removed, so they can be found and
    // turned back on.
    if (hidden) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
    }

    // The pointer-shaped id is what keeps ImGui's per-row state attached to the
    // right node when siblings are added or removed; the label alone would not
    // be unique.
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<usize>(id)), flags, "%s",
                                        scene.node(id).name.c_str());

    if (hidden) {
        ImGui::PopStyleColor();
    }

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        state.selected = id;
    }

    // Both actions are deferred until after the widgets are built: mutating the
    // scene mid-row would leave ImGui reading a freed name, and would change
    // the children vector being walked below.
    bool destroyRequested = false;
    bool addChildRequested = false;

    if (ImGui::BeginPopupContextItem()) {
        state.selected = id;
        if (ImGui::MenuItem(hidden ? "Show" : "Hide")) {
            // Visibility is inherited, so this hides the whole subtree.
            scene.node(id).visible = hidden;
        }
        if (ImGui::MenuItem("Add child")) {
            addChildRequested = true;
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
    if (destroyRequested) {
        if (state.selected == id) {
            state.selected = kInvalidNode;
        }
        scene.destroyNode(id);
    }
}

} // namespace

void drawDockspace(EditorState& state) {
    // A borderless window covering the whole viewport, used only as a host for
    // docked panels.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                   ImGuiWindowFlags_NoBackground;

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

    // PassthruCentralNode leaves the middle of the dockspace unpainted, so the
    // scene rendered underneath shows through. That is what makes the centre
    // read as a viewport without an off-screen render target existing yet.
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Hierarchy", nullptr, &state.showHierarchy);
            ImGui::MenuItem("Inspector", nullptr, &state.showInspector);
            ImGui::MenuItem("Statistics", nullptr, &state.showStats);
            ImGui::Separator();
            if (ImGui::MenuItem("Reset layout")) {
                state.resetLayoutRequested = true;
            }
            ImGui::MenuItem("ImGui demo", nullptr, &state.showImGuiDemo);
            ImGui::EndMenu();
        }

        // Right-aligned hint, since the controls are not discoverable otherwise.
        const char* hint = "right mouse: look   |   WASD: move   |   shift: sprint";
        const f32 hintWidth = ImGui::CalcTextSize(hint).x;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - hintWidth - ImGui::GetStyle().WindowPadding.x * 2.0f);
        ImGui::TextDisabled("%s", hint);

        ImGui::EndMenuBar();
    }

    ImGui::End();
}

void drawHierarchyPanel(EditorState& state, Scene& scene) {
    if (!state.showHierarchy) {
        return;
    }

    if (ImGui::Begin("Hierarchy", &state.showHierarchy)) {
        if (ImGui::Button("Add node")) {
            state.selected = scene.createNode("node");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%zu nodes", scene.nodeCount());
        ImGui::Separator();

        // The root itself is not shown - it is an implementation detail, and an
        // always-present row that cannot be deleted would only be in the way.
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

void drawInspectorPanel(EditorState& state, Scene& scene, const Renderer& renderer) {
    if (!state.showInspector) {
        return;
    }

    if (ImGui::Begin("Inspector", &state.showInspector)) {
        if (state.selected == kInvalidNode || !scene.isAlive(state.selected)) {
            ImGui::TextDisabled("Nothing selected.");
            ImGui::End();
            return;
        }

        Node& node = scene.node(state.selected);

        // A fixed-size buffer rather than binding std::string directly: ImGui's
        // text field writes into raw memory, and the string would have to be
        // resized from a callback.
        char nameBuffer[128];
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", node.name.c_str());
        if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer))) {
            node.name = nameBuffer;
        }

        ImGui::Checkbox("Visible", &node.visible);

        ImGui::SeparatorText("Transform");

        // DragFloat3 edits in place and reports whether it changed, so the
        // scene is written only on an actual edit.
        ImGui::DragFloat3("Position", &node.transform.position.x, 0.02f);

        // Rotation round-trips through Euler angles for editing. Only written
        // back when touched, so an untouched quaternion never loses precision
        // to the conversion.
        Vec3 euler = toEulerDegrees(node.transform.rotation);
        if (ImGui::DragFloat3("Rotation", &euler.x, 0.5f)) {
            node.transform.rotation = fromEulerDegrees(euler);
        }

        ImGui::DragFloat3("Scale", &node.transform.scale.x, 0.01f, 0.001f, 1000.0f);

        if (ImGui::Button("Reset transform")) {
            node.transform = Transform{};
        }

        ImGui::SeparatorText("Renderable");
        if (node.mesh.valid()) {
            ImGui::Text("Mesh: #%u (%u indices)", node.mesh.index,
                        renderer.resources().mesh(node.mesh).indexCount());
        } else {
            ImGui::TextDisabled("No mesh - this node only groups others.");
        }

        if (node.material.valid() && renderer.resources().has(node.material)) {
            ImGui::Text("Material: %s", renderer.resources().material(node.material).name.c_str());
        } else {
            ImGui::TextDisabled("Material: fallback");
        }

        ImGui::SeparatorText("Hierarchy");
        ImGui::Text("Parent: %s",
                    node.parent == kRootNode ? "(scene root)" : scene.node(node.parent).name.c_str());
        ImGui::Text("Children: %zu", node.children.size());
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
        scene.forEachDrawable([&drawables](const Node&, const Mat4&) { ++drawables; });
        ImGui::Text("Draw calls: %zu", drawables);

        ImGui::SeparatorText("Resources");
        ImGui::Text("Meshes: %zu", renderer.resources().meshCount());
        ImGui::Text("Materials: %zu", renderer.resources().materialCount());
        ImGui::Text("Textures: %zu", renderer.resources().textureCount());

        ImGui::SeparatorText("Camera");
        const Camera& camera = renderer.camera();
        ImGui::Text("Position: %.2f, %.2f, %.2f", static_cast<f64>(camera.position.x),
                    static_cast<f64>(camera.position.y), static_cast<f64>(camera.position.z));
        ImGui::Text("Yaw / pitch: %.1f / %.1f", static_cast<f64>(camera.yaw),
                    static_cast<f64>(camera.pitch));
    }
    ImGui::End();
}

} // namespace fumar
