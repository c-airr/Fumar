#include "panels.hpp"

#include "fumar/core/math.hpp"
#include "fumar/platform/paths.hpp"
#include "fumar/render/renderer.hpp"
#include "fumar/script/script_engine.hpp"
#include "fumar/ui/imgui_layer.hpp"

#include <ImGuizmo.h>
#include <imgui.h>
// DockBuilder is not part of the public API - it lives in imgui_internal.h and
// is the only way to arrange panels from code rather than by dragging them.
#include <imgui_internal.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fumar {
namespace {

// Every colour named here goes through uiColor: the window is presented in an
// sRGB format, so a value written straight out comes back lighter than it was
// picked. See fumar/ui/imgui_layer.hpp.
const ImVec4 kAccent = uiColor(0.659f, 0.373f, 0.173f);
const ImVec4 kAccentBright = uiColor(1.0f, 0.72f, 0.45f);
const ImVec4 kError = uiColor(1.0f, 0.45f, 0.35f);
const ImVec4 kOk = uiColor(0.55f, 0.85f, 0.45f);

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
        ImGui::PushStyleColor(ImGuiCol_Border, kAccentBright);
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

    // The right-hand column first: what the scene contains, and what the
    // selected thing is. Splitting it off before anything else means the
    // viewport ends up with whatever is left, which is how it should be.
    ImGuiID rightTop = ImGui::DockBuilderSplitNode(remainder, ImGuiDir_Right, 0.24f, nullptr,
                                                   &remainder);
    const ImGuiID rightBottom =
        ImGui::DockBuilderSplitNode(rightTop, ImGuiDir_Down, 0.58f, nullptr, &rightTop);

    // Then a strip along the bottom of what remains, under the viewport.
    const ImGuiID bottom =
        ImGui::DockBuilderSplitNode(remainder, ImGuiDir_Down, 0.30f, nullptr, &remainder);

    ImGui::DockBuilderDockWindow("World Outliner", rightTop);
    ImGui::DockBuilderDockWindow("Details", rightBottom);

    // Docked in reverse order of interest: the last one to arrive is the tab
    // that opens, and Content is what you reach for most.
    ImGui::DockBuilderDockWindow("Statistics", bottom);
    ImGui::DockBuilderDockWindow("Scripts", bottom);
    ImGui::DockBuilderDockWindow("Content", bottom);

    // Everything not carved off above.
    ImGui::DockBuilderDockWindow("Viewport", remainder);

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
        ImGui::PushStyleColor(ImGuiCol_Text, uiColor(0.45f, 0.45f, 0.47f));
    } else if (id == state.hovered) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAccentBright);
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
        state.history.record(scene, state.selected);
        state.selected = scene.createNode("node", id);
    }
    if (duplicateRequested) {
        state.history.record(scene, state.selected);
        const NodeId copy = scene.duplicateNode(id);
        if (copy != kInvalidNode) {
            state.selected = copy;
        }
    }
    if (destroyRequested) {
        state.history.record(scene, state.selected);
        if (state.selected == id) {
            state.selected = kInvalidNode;
        }
        scene.destroyNode(id);
    }
}

} // namespace

void drawDockspace(EditorState& state, const std::filesystem::path& sceneDirectory) {
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
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New scene")) {
                state.newSceneRequested = true;
            }

            if (ImGui::BeginMenu("Open")) {
                std::error_code ec;
                bool anyListed = false;

                if (std::filesystem::exists(sceneDirectory, ec)) {
                    for (const auto& entry : std::filesystem::directory_iterator(sceneDirectory, ec)) {
                        if (!entry.is_regular_file() || entry.path().extension() != ".fumar") {
                            continue;
                        }
                        anyListed = true;
                        if (ImGui::MenuItem(entry.path().filename().string().c_str())) {
                            state.openRequested = true;
                            state.openPath = entry.path().string();
                        }
                    }
                }

                if (!anyListed) {
                    ImGui::TextDisabled("no saved scenes");
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S")) {
                state.saveRequested = true;
            }
            ImGui::EndMenu();
        }

        // Edit exists mostly to advertise the shortcuts. Every entry is
        // reachable from the keyboard, and a menu is where people look to find
        // out which key does it.
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, state.history.canUndo())) {
                state.undoRequested = true;
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, state.history.canRedo())) {
                state.redoRequested = true;
            }
            ImGui::Separator();

            const bool hasSelection = state.selected != kInvalidNode;
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, hasSelection)) {
                state.copyRequested = true;
            }
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, !state.clipboard.empty())) {
                state.pasteRequested = true;
            }
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSelection)) {
                state.duplicateRequested = true;
            }
            if (ImGui::MenuItem("Delete", "Del", false, hasSelection)) {
                state.deleteRequested = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Window")) {
            ImGui::MenuItem("World Outliner", nullptr, &state.showOutliner);
            ImGui::MenuItem("Details", nullptr, &state.showDetails);
            ImGui::MenuItem("Content", nullptr, &state.showContent);
            ImGui::MenuItem("Scripts", nullptr, &state.showScripts);
            ImGui::MenuItem("Statistics", nullptr, &state.showStats);
            ImGui::Separator();
            if (ImGui::MenuItem("Reset layout")) {
                state.resetLayoutRequested = true;
            }
            ImGui::MenuItem("ImGui demo", nullptr, &state.showImGuiDemo);
            ImGui::EndMenu();
        }

        // The file being edited, in the middle of the bar where a title would
        // normally sit.
        ImGui::TextDisabled("|");
        if (state.scenePath.empty()) {
            ImGui::TextDisabled("untitled");
        } else {
            ImGui::TextDisabled("%s", std::filesystem::path(state.scenePath).filename().string().c_str());
        }

        if (state.saveFlashSeconds > 0.0f) {
            ImGui::SameLine();
            ImGui::TextColored(kOk, "saved");
        }

        const char* hint =
            "right mouse: look  |  WASD: move  |  Q W E R: tools  |  Ctrl+D: duplicate  |  F5: compile";
        const f32 hintWidth = ImGui::CalcTextSize(hint).x;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - hintWidth - ImGui::GetStyle().WindowPadding.x * 2.0f);
        ImGui::TextDisabled("%s", hint);

        ImGui::EndMenuBar();
    }

    ImGui::End();
}

void drawViewportPanel(EditorState& state, Renderer& renderer, ScriptEngine& scripts) {
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

    // Compile and Play sit at the right end of the toolbar, the way a build
    // button does in every editor.
    const f32 rightGroupWidth = 190.0f;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - rightGroupWidth + ImGui::GetCursorPosX());

    if (toolButton("Compile", false, "Recompile every script (F5)")) {
        scripts.compileAll();
    }
    ImGui::SameLine();

    if (toolButton(state.scriptsRunning ? "Stop" : "Play", state.scriptsRunning,
                   "Run the scripts attached to nodes")) {
        state.scriptsRunning = !state.scriptsRunning;
        if (state.scriptsRunning) {
            // Fresh run: on_start fires again for everything.
            scripts.restart();
        }
    }

    if (!scripts.errors().empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kError, "%zu error(s)", scripts.errors().size());
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

        const bool wasUsing = ImGuizmo::IsUsing();

        if (ImGuizmo::Manipulate(&view.columns[0].x, &projection.columns[0].x, operation, mode,
                                 &world.columns[0].x, nullptr,
                                 state.snapEnabled ? snapVector : nullptr)) {
            // Recorded on the FRAME THE DRAG BEGINS, not while it continues.
            // A gizmo pull fires this every frame it moves, and one undo step
            // per frame would mean fifty presses of Ctrl+Z to take back one
            // drag - which is not undo, it is a rewind.
            if (!wasUsing) {
                state.history.record(scene, state.selected);
            }

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
            state.history.record(scene, state.selected);
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

void drawDetailsPanel(EditorState& state, Scene& scene, Renderer& renderer,
                      const ScriptEngine& scripts) {
    if (!state.showDetails) {
        return;
    }

    if (ImGui::Begin("Details", &state.showDetails)) {
        // With nothing selected this is where the world itself is edited. An
        // inspector showing "select something" is a panel-sized apology.
        if (state.selected == kInvalidNode || !scene.isAlive(state.selected)) {
            ImGui::TextDisabled("World");
            ImGui::Spacing();
            drawWorldSettings(renderer);
            ImGui::End();
            return;
        }

        Node& node = scene.node(state.selected);

        // Called after a widget to take an undo step at the moment editing
        // STARTS. IsItemActivated fires on the frame the widget is first
        // grabbed, before any value has moved, so what gets recorded is the
        // state to come back to - and a drag that spans fifty frames still
        // costs exactly one step.
        const auto recordOnEdit = [&] {
            if (ImGui::IsItemActivated()) {
                state.history.record(scene, state.selected);
            }
        };

        // A fixed-size buffer rather than binding std::string directly: ImGui
        // text fields write into raw memory, and a string would have to be
        // resized from a callback.
        char nameBuffer[128];
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", node.name.c_str());
        if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer))) {
            node.name = nameBuffer;
        }
        recordOnEdit();

        if (ImGui::Checkbox("Visible", &node.visible)) {
            state.history.record(scene, state.selected);
        }

        ImGui::SeparatorText("Transform");

        ImGui::DragFloat3("Location", &node.transform.position.x, 0.02f);
        recordOnEdit();

        // Rotation round-trips through Euler angles for editing. Written back
        // only when touched, so an untouched quaternion never loses precision
        // to the conversion.
        Vec3 euler = toEulerDegrees(node.transform.rotation);
        if (ImGui::DragFloat3("Rotation", &euler.x, 0.5f)) {
            node.transform.rotation = fromEulerDegrees(euler);
        }
        recordOnEdit();

        ImGui::DragFloat3("Scale", &node.transform.scale.x, 0.01f, 0.001f, 1000.0f);
        recordOnEdit();

        if (ImGui::Button("Reset transform")) {
            state.history.record(scene, state.selected);
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
            Material& material = renderer.resources().material(node.material);
            ImGui::Text("Material: %s", material.name.c_str());

            // Editing here changes the material for every object using it. That
            // is not a bug to work around: a material is a shared asset, and
            // pretending otherwise would mean silently cloning it on the first
            // edit and leaving the user with two things named the same.
            ImGui::ColorEdit3("Base colour", &material.baseColorFactor.x,
                              ImGuiColorEditFlags_Float);

            ImGui::SliderFloat("Metallic", &material.metallic, 0.0f, 1.0f);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("0 = plastic, stone, wood.  1 = metal.\n"
                                  "Metals have no diffuse colour: they tint what they reflect.");
            }

            ImGui::SliderFloat("Roughness", &material.roughness, 0.0f, 1.0f);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("0 = mirror, 1 = fully diffuse.\n"
                                  "Decides how wide the highlight is.");
            }
        } else {
            ImGui::TextDisabled("Material: default");
        }

        ImGui::SeparatorText("Script");

        // A combo over what actually compiled, rather than a free text field:
        // a typo in a script name would otherwise fail silently at runtime.
        const std::string current = node.script.empty() ? "(none)" : node.script;
        if (ImGui::BeginCombo("Script", current.c_str())) {
            if (ImGui::Selectable("(none)", node.script.empty())) {
                state.history.record(scene, state.selected);
                node.script.clear();
            }
            for (const std::string& name : scripts.scriptNames()) {
                if (ImGui::Selectable(name.c_str(), node.script == name)) {
                    state.history.record(scene, state.selected);
                    node.script = name;
                }
            }
            ImGui::EndCombo();
        }

        if (!node.script.empty() && !scripts.has(node.script)) {
            ImGui::TextColored(kError, "'%s' is not compiled",
                               node.script.c_str());
        }

        ImGui::SeparatorText("Hierarchy");
        ImGui::Text("Parent: %s",
                    node.parent == kRootNode ? "(scene root)" : scene.node(node.parent).name.c_str());
        ImGui::Text("Children: %zu", node.children.size());
    }
    ImGui::End();
}

void drawWorldSettings(Renderer& renderer) {
    {
        Environment& env = renderer.environment();

        // Four points on the same set of dials, not four different renderers.
        // Worth having as buttons because the difference between them is
        // entirely in numbers that are hard to guess from a cold start.
        ImGui::SeparatorText("Presets");
        if (ImGui::Button("Noon")) {
            env = Environment{};
        }
        ImGui::SameLine();
        if (ImGui::Button("Golden hour")) {
            env.sunElevationDegrees = 6.0f;
            env.sunColor = Vec3{1.0f, 0.72f, 0.42f};
            env.sunIntensity = 4.0f;
            env.skyZenithColor = Vec3{0.18f, 0.30f, 0.62f};
            env.skyHorizonColor = Vec3{0.95f, 0.62f, 0.38f};
            env.groundColor = Vec3{0.20f, 0.16f, 0.13f};
            env.skyIntensity = 0.6f;
            env.exposure = 0.55f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Overcast")) {
            // The sun is still there, it is just no longer the dominant light:
            // raise the sky and the shadows fill in until nothing casts one.
            env.sunElevationDegrees = 60.0f;
            env.sunColor = Vec3{0.92f, 0.93f, 0.95f};
            env.sunIntensity = 0.9f;
            env.skyZenithColor = Vec3{0.60f, 0.63f, 0.68f};
            env.skyHorizonColor = Vec3{0.72f, 0.74f, 0.77f};
            env.groundColor = Vec3{0.30f, 0.30f, 0.30f};
            env.skyIntensity = 1.3f;
            env.exposure = 0.5f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Night")) {
            env.sunElevationDegrees = 34.0f;
            env.sunColor = Vec3{0.68f, 0.76f, 1.0f};
            env.sunIntensity = 0.5f;
            env.sunAngularRadiusDegrees = 0.3f;
            env.skyZenithColor = Vec3{0.015f, 0.025f, 0.06f};
            env.skyHorizonColor = Vec3{0.06f, 0.08f, 0.14f};
            env.groundColor = Vec3{0.02f, 0.02f, 0.03f};
            env.skyIntensity = 1.0f;
            env.exposure = 1.6f;
        }

        ImGui::SeparatorText("Sun");

        // Elevation goes below zero deliberately: dragging the sun under the
        // horizon is how you get night, and it should be one slider away.
        ImGui::SliderFloat("Elevation", &env.sunElevationDegrees, -15.0f, 90.0f, "%.1f deg");
        ImGui::SliderFloat("Azimuth", &env.sunAzimuthDegrees, 0.0f, 360.0f, "%.1f deg");
        ImGui::ColorEdit3("Sun colour", &env.sunColor.x, ImGuiColorEditFlags_Float);
        ImGui::DragFloat("Sun intensity", &env.sunIntensity, 0.02f, 0.0f, 40.0f);
        ImGui::DragFloat("Sun size", &env.sunAngularRadiusDegrees, 0.01f, 0.05f, 15.0f, "%.2f deg");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Half-angle of the disc. The real sun is 0.27 degrees.");
        }

        ImGui::SeparatorText("Sky");
        ImGui::ColorEdit3("Zenith", &env.skyZenithColor.x, ImGuiColorEditFlags_Float);
        ImGui::ColorEdit3("Horizon", &env.skyHorizonColor.x, ImGuiColorEditFlags_Float);
        ImGui::ColorEdit3("Ground", &env.groundColor.x, ImGuiColorEditFlags_Float);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Also the light bouncing up off the ground onto\n"
                              "the underside of everything. Set it to black and\n"
                              "objects start to look like they are floating.");
        }
        ImGui::DragFloat("Sky intensity", &env.skyIntensity, 0.01f, 0.0f, 8.0f);

        ImGui::SeparatorText("Ray tracing");
        if (renderer.rayTracingSupported()) {
            ImGui::SliderFloat("Shadows", &env.shadowStrength, 0.0f, 1.0f);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Rays traced from each lit pixel towards the sun.\n"
                                  "Their softness comes from Sun size above: the sun\n"
                                  "is a disc, not a point, which is why real shadows\n"
                                  "blur the further they fall from what casts them.");
            }

            ImGui::SliderFloat("Occlusion", &env.occlusionStrength, 0.0f, 1.0f);
            ImGui::DragFloat("Occlusion reach", &env.occlusionRadius, 0.02f, 0.05f, 20.0f, "%.2f m");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How far the occlusion rays look for something\n"
                                  "blocking the sky. Short values darken creases and\n"
                                  "contact points; long ones start doing the sun\n"
                                  "shadow's job, badly.");
            }
        } else {
            ImGui::TextDisabled("This GPU has no VK_KHR_ray_query.");
            ImGui::TextDisabled("Shadows and occlusion are unavailable.");
        }

        ImGui::SeparatorText("Camera");
        ImGui::DragFloat("Exposure", &env.exposure, 0.005f, 0.01f, 8.0f);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The scene is rendered in floating point, where the sun\n"
                              "is worth dozens and a shadow a fraction of one. This is\n"
                              "what decides where that range lands on the display.");
        }

        // The direction the two angles work out to. Not editable - it is
        // derived - but seeing it move while dragging is what makes the
        // relationship between the sliders and the world obvious.
        const Vec3 direction = env.sunDirection();
        ImGui::Spacing();
        ImGui::TextDisabled("sun vector: %.2f, %.2f, %.2f", static_cast<f64>(direction.x),
                            static_cast<f64>(direction.y), static_cast<f64>(direction.z));
        if (direction.y <= 0.0f) {
            ImGui::TextColored(kAccent, "below the horizon");
        }
    }
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
            state.history.record(scene, state.selected);
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

        ImGui::SeparatorText("Import");

        // Everything importable sitting next to the executable. Dragging a file
        // onto the window does the same thing; this is for what is already
        // there.
        const std::filesystem::path assetDir = executableDirectory() / "assets";
        std::error_code ec;
        bool anyListed = false;

        if (std::filesystem::exists(assetDir, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(assetDir, ec)) {
                if (!entry.is_regular_file() || !Renderer::isImportable(entry.path())) {
                    continue;
                }
                anyListed = true;

                if (ImGui::Selectable(entry.path().filename().string().c_str())) {
                    const NodeId imported = renderer.importAsset(entry.path());
                    if (imported != kInvalidNode) {
                        state.selected = imported;
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Click to import");
                }
            }
        }

        if (!anyListed) {
            ImGui::TextDisabled("Drop a .glb or an image onto the window,");
            ImGui::TextDisabled("or put one in assets/ next to the editor.");
        }

        ImGui::SeparatorText("Loaded");
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

void drawScriptsPanel(EditorState& state, ScriptEngine& scripts) {
    if (!state.showScripts) {
        return;
    }

    if (ImGui::Begin("Scripts", &state.showScripts)) {
        if (ImGui::Button("Compile", ImVec2(-1.0f, 0.0f))) {
            scripts.compileAll();
        }

        ImGui::TextDisabled("%s", scripts.directory().string().c_str());
        ImGui::Separator();

        if (scripts.scriptNames().empty()) {
            ImGui::TextDisabled("No scripts. Drop a .lua file in the folder above");
            ImGui::TextDisabled("and press Compile.");
        }

        for (const std::string& name : scripts.scriptNames()) {
            ImGui::BulletText("%s", name.c_str());
        }

        if (!scripts.errors().empty()) {
            ImGui::SeparatorText("Errors");

            // Wrapped, because a Lua error carries a file and line and is
            // routinely wider than the panel.
            ImGui::PushStyleColor(ImGuiCol_Text, kError);
            for (const ScriptError& error : scripts.errors()) {
                ImGui::TextWrapped("%s: %s", error.script.c_str(), error.message.c_str());
            }
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
}

} // namespace fumar
