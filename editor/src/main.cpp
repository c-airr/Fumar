#include "panels.hpp"

#include "fumar/core/log.hpp"
#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"
#include "fumar/platform/paths.hpp"
#include "fumar/platform/window.hpp"
#include "fumar/render/renderer.hpp"
#include "fumar/render/scene_io.hpp"
#include "fumar/scene/scene.hpp"
#include "fumar/script/script_engine.hpp"
#include "fumar/ui/imgui_layer.hpp"

#include <ImGuizmo.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>

using namespace fumar;

namespace {

/// Builds the scene the editor opens with.
///
/// A real editor would reopen the last project. Until scene files exist this
/// stands in for one - and an empty grey window is a poor first impression
/// either way.
void createStarterScene(Renderer& renderer) {
    Scene& scene = renderer.scene();

    // A restrained palette: everything is a shade of grey except the pillar,
    // which is warmed slightly so it reads as a different material without
    // turning the scene into a colour chart.
    const MaterialHandle floorMaterial =
        renderer.createMaterial("Floor", Vec4{0.34f, 0.345f, 0.36f, 1.0f});
    const MaterialHandle blockMaterial =
        renderer.createMaterial("Block", Vec4{0.60f, 0.61f, 0.63f, 1.0f});
    const MaterialHandle pillarMaterial =
        renderer.createMaterial("Pillar", Vec4{0.52f, 0.48f, 0.44f, 1.0f});

    const MeshHandle planeMesh = renderer.createPlaneMesh(14.0f);
    const MeshHandle cubeMesh = renderer.createCubeMesh();
    const MeshHandle cylinderMesh = renderer.createCylinderMesh(0.9f, 3.0f, 40);

    const NodeId floor = scene.createNode("Floor");
    scene.node(floor).mesh = planeMesh;
    scene.node(floor).material = floorMaterial;

    const NodeId pillar = scene.createNode("Pillar");
    scene.node(pillar).mesh = cylinderMesh;
    scene.node(pillar).material = pillarMaterial;
    // Wired up so a fresh editor has something to press Play on.
    scene.node(pillar).script = "spin";
    // Half its height plus a hair: sitting the bottom cap exactly on the floor
    // plane makes the two surfaces coplanar, and the depth test then picks
    // between them per pixel - the flickering black patch known as z-fighting.
    scene.node(pillar).transform.position = Vec3{0.0f, 1.502f, 0.0f};

    // Grouped under one node, so the whole arrangement can be moved or hidden
    // with a single selection - which is what a hierarchy is for.
    const NodeId blocks = scene.createNode("Blocks");

    struct BlockLayout {
        const char* name;
        Vec3 position;
        Vec3 scale;
        f32 yawDegrees;
    };

    const BlockLayout layout[]{
        {"Block A", {-4.0f, 0.5f, 2.5f}, {1.0f, 1.0f, 1.0f}, 0.0f},
        {"Block B", {-2.6f, 0.35f, -3.2f}, {0.7f, 0.7f, 0.7f}, 25.0f},
        {"Block C", {3.6f, 0.75f, 1.4f}, {1.5f, 1.5f, 1.5f}, -15.0f},
        {"Block D", {4.4f, 0.3f, -2.8f}, {0.6f, 0.6f, 0.6f}, 40.0f},
        {"Step", {0.0f, 0.2f, 4.2f}, {3.0f, 0.4f, 1.2f}, 0.0f},
    };

    for (const BlockLayout& block : layout) {
        const NodeId id = scene.createNode(block.name, blocks);

        // Taken after createNode, which may have reallocated the node storage.
        Node& node = scene.node(id);
        node.mesh = cubeMesh;
        node.material = blockMaterial;
        node.transform.position = block.position;
        node.transform.scale = block.scale;
        node.transform.rotation = fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, radians(block.yawDegrees));
    }

    // One block bobs, so Play visibly does two different things at once.
    if (scene.isAlive(blocks) && !scene.node(blocks).children.empty()) {
        scene.node(scene.node(blocks).children.front()).script = "bob";
    }

    const std::filesystem::path modelPath = executableDirectory() / "assets" / "DamagedHelmet.glb";
    if (std::filesystem::exists(modelPath)) {
        const NodeId model = renderer.loadModel(modelPath);
        if (model != kInvalidNode) {
            scene.node(model).transform.position = Vec3{0.0f, 4.2f, 0.0f};
        }
    }

    FUMAR_INFO("starter scene: {} nodes, {} meshes, {} materials", scene.nodeCount(),
               renderer.resources().meshCount(), renderer.resources().materialCount());
}

/// Keyboard shortcuts that apply when no text field has focus.
void handleShortcuts(EditorState& state, Scene& scene) {
    if (ImGui::GetIO().WantTextInput) {
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
        state.gizmoMode = GizmoMode::Select;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
        state.gizmoMode = GizmoMode::Translate;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
        state.gizmoMode = GizmoMode::Rotate;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        state.gizmoMode = GizmoMode::Scale;
    }

    // Ctrl+D duplicates, matching every other editor. The copy is selected
    // immediately, so the gizmo is already on it and it can be dragged off the
    // original without another click.
    //
    // Written as a modifier check plus a key press rather than with
    // IsKeyChordPressed: chords go through ImGui shortcut routing, which asks
    // which window owns the shortcut, and a global editor binding owned by no
    // particular panel is exactly the case that routing declines to deliver.
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) &&
        state.selected != kInvalidNode && scene.isAlive(state.selected)) {
        const NodeId copy = scene.duplicateNode(state.selected);
        if (copy != kInvalidNode) {
            state.selected = copy;
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && state.selected != kInvalidNode &&
        scene.isAlive(state.selected)) {
        scene.destroyNode(state.selected);
        state.selected = kInvalidNode;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        state.selected = kInvalidNode;
    }
}

/// Turns the cursor position in the viewport into a selection.
///
/// Runs every frame for the hover highlight, and commits to a selection only on
/// a click. Doing the pick on hover as well is what makes objects light up as
/// the cursor passes over them.
void updatePicking(EditorState& state, Renderer& renderer, bool cameraActive) {
    if (!state.viewportHovered || cameraActive) {
        state.hovered = kInvalidNode;
        renderer.setHighlighted(kInvalidNode);
        return;
    }

    const Extent2D size = renderer.viewportExtent();
    const f32 aspect = size.height > 0 ? static_cast<f32>(size.width) / static_cast<f32>(size.height) : 1.0f;

    const Ray ray = renderer.camera().rayThrough(state.viewportCursor, aspect);
    state.hovered = renderer.pickNode(ray);
    renderer.setHighlighted(state.hovered);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // Clicking empty space clears the selection rather than keeping it,
        // matching what the outliner does and what people expect.
        state.selected = state.hovered;
    }
}

} // namespace

int main() {
    using Clock = std::chrono::steady_clock;

    FUMAR_INFO("fumar editor starting");

    Window window(WindowDesc{
        .title = "fumar editor",
        .width = 1600,
        .height = 900,
        .resizable = true,
    });

    Renderer renderer(window);
    ImGuiLayer ui(window, renderer);

    // Scripts live next to the executable, beside the assets. Compiled once at
    // startup so anything already written is available immediately.
    ScriptEngine scripts(executableDirectory() / "scripts");
    scripts.compileAll();

    // The renderer records this after the scene, into the window - the scene
    // itself goes to an off-screen image that the viewport panel displays.
    renderer.setOverlay([&ui](vk::CommandBuffer cmd) { ui.record(cmd); });

    createStarterScene(renderer);
    renderer.camera().position = Vec3{7.5f, 5.5f, 10.0f};
    renderer.camera().yaw = -125.0f;
    renderer.camera().pitch = -20.0f;

    const std::filesystem::path sceneDirectory = executableDirectory() / "scenes";

    EditorState state;
    state.viewportTexture = ui.registerTexture(renderer.viewportImageView(), renderer.viewportSampler());

    auto lastFrameTime = Clock::now();

    while (!window.shouldClose()) {
        window.pumpEvents();

        if (window.minimized()) {
            window.waitEvents(100);
            lastFrameTime = Clock::now();
            continue;
        }

        const auto now = Clock::now();
        const f32 deltaSeconds = std::min(std::chrono::duration<f32>(now - lastFrameTime).count(), 0.1f);
        lastFrameTime = now;

        // --- viewport sizing ------------------------------------------------
        // Uses the size the panel reported LAST frame, and happens before any
        // widget is described this frame. That ordering is not optional:
        // resizing creates a new image and frees the descriptor ImGui was given
        // for the old one, so doing it after ImGui::Image had already recorded
        // that descriptor would leave a freed handle inside the draw list, and
        // the validation layers flag it as an invalid descriptor set on submit.
        //
        // Costs one frame of latency after a resize - the panel is drawn at the
        // new size with an image still at the old one - which is invisible next
        // to the alternative.
        if (renderer.resizeViewport(state.viewportSize)) {
            ui.unregisterTexture(state.viewportTexture);
            state.viewportTexture =
                ui.registerTexture(renderer.viewportImageView(), renderer.viewportSampler());
        }

        // --- interface ------------------------------------------------------
        ui.beginFrame();
        ImGuizmo::BeginFrame();

        drawDockspace(state, sceneDirectory);
        drawViewportPanel(state, renderer, scripts);
        drawOutlinerPanel(state, renderer.scene());
        drawDetailsPanel(state, renderer.scene(), renderer, scripts);
        drawContentPanel(state, renderer);
        drawScriptsPanel(state, scripts);
        drawStatsPanel(state, renderer.scene(), renderer);

        if (state.showImGuiDemo) {
            // Reachable from the Window menu: the fastest reference for what a
            // widget looks like and how it is called.
            ImGui::ShowDemoWindow(&state.showImGuiDemo);
        }

        handleShortcuts(state, renderer.scene());

        // F5 recompiles, the way every editor with a build step does it.
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false) && !ImGui::GetIO().WantTextInput) {
            scripts.compileAll();
        }

        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) &&
            !ImGui::GetIO().WantTextInput) {
            state.saveRequested = true;
        }

        if (state.saveFlashSeconds > 0.0f) {
            state.saveFlashSeconds -= deltaSeconds;
        }

        // Scripts run only while playing, so an object being positioned by hand
        // does not fight a script moving it.
        if (state.scriptsRunning) {
            scripts.update(renderer.scene(), deltaSeconds);
        }

        // --- camera ---------------------------------------------------------
        // Flying is allowed only from inside the viewport, so dragging in a
        // panel never moves the view. Once the cursor is captured the check is
        // skipped, because in that mode ImGui no longer receives meaningful
        // positions and would report the cursor as being nowhere.
        const bool cameraActive =
            window.relativeMouse() ||
            (state.viewportHovered && window.mouseButtonDown(MouseButton::Right));

        if (cameraActive) {
            renderer.camera().update(window, deltaSeconds);
        } else if (window.relativeMouse()) {
            window.setRelativeMouse(false);
        }

        updatePicking(state, renderer, cameraActive);
        renderer.setSelected(state.selected);

        ui.endFrame();
        renderer.drawFrame();

        // --- scene file actions ----------------------------------------------
        // Deferred to after the frame on purpose: loading replaces the very
        // nodes the panels were describing, and destroying them mid-frame would
        // leave ImGui holding freed strings.
        if (state.saveRequested) {
            state.saveRequested = false;
            if (state.scenePath.empty()) {
                state.scenePath = (sceneDirectory / "untitled.fumar").string();
            }
            if (saveScene(renderer, state.scenePath)) {
                state.saveFlashSeconds = 1.5f;
            }
        }

        if (state.openRequested) {
            state.openRequested = false;
            if (loadScene(renderer, state.openPath)) {
                state.scenePath = state.openPath;
                state.selected = kInvalidNode;
                state.hovered = kInvalidNode;
                scripts.restart();
            }
        }

        if (state.newSceneRequested) {
            state.newSceneRequested = false;
            renderer.resetScene();
            createStarterScene(renderer);
            state.scenePath.clear();
            state.selected = kInvalidNode;
            state.hovered = kInvalidNode;
            scripts.restart();
        }
    }

    // The last submitted frame may still be executing, and its command buffer
    // references this descriptor set. Freeing it first is a validation error
    // and, on a real driver, a use-after-free.
    renderer.waitIdle();
    ui.unregisterTexture(state.viewportTexture);

    FUMAR_INFO("fumar editor shutting down");
    return 0;
}
