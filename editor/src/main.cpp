#include "panels.hpp"

#include "fumar/core/log.hpp"
#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"
#include "fumar/platform/paths.hpp"
#include "fumar/platform/window.hpp"
#include "fumar/render/renderer.hpp"
#include "fumar/scene/scene.hpp"
#include "fumar/ui/imgui_layer.hpp"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>

using namespace fumar;

namespace {

/// Puts something in the scene so a fresh launch is not an empty grey window.
///
/// A real editor would open the last project instead. Until scene files exist
/// (they are the next step), this stands in for one.
void createStarterScene(Renderer& renderer) {
    Scene& scene = renderer.scene();

    const MeshHandle groundMesh = renderer.createPlaneMesh(12.0f, 12.0f);
    const NodeId ground = scene.createNode("ground");
    scene.node(ground).mesh = groundMesh;

    const MeshHandle cubeMesh = renderer.createCubeMesh();
    const NodeId group = scene.createNode("cubes");
    scene.node(group).transform.position = Vec3{0.0f, 0.5f, 0.0f};

    for (u32 i = 0; i < 3; ++i) {
        const NodeId cube = scene.createNode(std::format("cube_{}", i), group);

        // Taken after createNode, which may have reallocated the node storage.
        Node& node = scene.node(cube);
        node.mesh = cubeMesh;
        node.transform.position = Vec3{static_cast<f32>(i) * 2.0f - 2.0f, 0.0f, 0.0f};
        node.transform.scale = Vec3{0.8f, 0.8f, 0.8f};
    }

    const std::filesystem::path modelPath = executableDirectory() / "assets" / "DamagedHelmet.glb";
    if (std::filesystem::exists(modelPath)) {
        const NodeId model = renderer.loadModel(modelPath);
        if (model != kInvalidNode) {
            scene.node(model).transform.position = Vec3{0.0f, 2.0f, -3.0f};
        }
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

    // The renderer records this after the scene, inside the same render pass.
    renderer.setOverlay([&ui](vk::CommandBuffer cmd) { ui.record(cmd); });

    createStarterScene(renderer);
    renderer.camera().position = Vec3{0.0f, 3.0f, 9.0f};

    EditorState state;
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

        // --- interface ------------------------------------------------------
        ui.beginFrame();

        drawDockspace(state);
        drawHierarchyPanel(state, renderer.scene());
        drawInspectorPanel(state, renderer.scene(), renderer);
        drawStatsPanel(state, renderer.scene(), renderer);

        if (state.showImGuiDemo) {
            // Kept reachable from the View menu: it is the fastest reference for
            // what a widget looks like and how it is called.
            ImGui::ShowDemoWindow(&state.showImGuiDemo);
        }

        ui.endFrame();

        // --- camera ---------------------------------------------------------
        // Skipped while the interface has the mouse or keyboard, so dragging a
        // slider does not also fly the camera across the scene. Once the camera
        // has grabbed the cursor it keeps it, because in that mode ImGui no
        // longer receives meaningful mouse positions.
        const bool uiHasInput = ui.wantsMouse() || ui.wantsKeyboard();
        if (!uiHasInput || window.relativeMouse()) {
            renderer.camera().update(window, deltaSeconds);
        }

        renderer.drawFrame();
    }

    FUMAR_INFO("fumar editor shutting down");
    return 0;
}
