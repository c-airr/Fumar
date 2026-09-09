#include "fumar/core/log.hpp"
#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"
#include "fumar/platform/paths.hpp"
#include "fumar/platform/window.hpp"
#include "fumar/render/renderer.hpp"
#include "fumar/scene/scene.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>

using namespace fumar;

namespace {

/// Holds the ids the update loop needs to animate.
struct SandboxScene {
    NodeId pivot = kInvalidNode;
    std::array<NodeId, 3> cubes{kInvalidNode, kInvalidNode, kInvalidNode};
    NodeId model = kInvalidNode;
};

/// Builds the demo scene.
///
/// This is application code, not engine code: it only touches the scene tree
/// and the content factories. The same calls are what an editor will make when
/// the user drags a mesh into the viewport.
SandboxScene buildScene(Renderer& renderer) {
    SandboxScene ids;
    Scene& scene = renderer.scene();

    const MeshHandle groundMesh = renderer.createPlaneMesh(12.0f, 12.0f);
    const MeshHandle cubeMesh = renderer.createCubeMesh();

    const NodeId ground = scene.createNode("ground");
    scene.node(ground).mesh = groundMesh;

    // The cubes hang off a pivot rather than sitting at the scene root, so
    // rotating one node carries all three around it. That is the whole point of
    // keeping a hierarchy instead of a flat list.
    ids.pivot = scene.createNode("pivot");
    scene.node(ids.pivot).transform.position = Vec3{0.0f, 1.0f, 0.0f};

    for (u32 i = 0; i < ids.cubes.size(); ++i) {
        const NodeId cube = scene.createNode(std::format("cube_{}", i), ids.pivot);
        ids.cubes[i] = cube;

        // Careful: createNode above may have reallocated the node storage, so
        // the reference is taken after it, not before.
        Node& node = scene.node(cube);
        node.mesh = cubeMesh;

        const f32 angle = kTwoPi * static_cast<f32>(i) / static_cast<f32>(ids.cubes.size());
        node.transform.position = Vec3{std::cos(angle) * 3.0f, 0.0f, std::sin(angle) * 3.0f};
        node.transform.scale = Vec3{0.7f, 0.7f, 0.7f};
    }

    const std::filesystem::path modelPath = executableDirectory() / "assets" / "DamagedHelmet.glb";
    if (std::filesystem::exists(modelPath)) {
        ids.model = renderer.loadModel(modelPath);
        if (ids.model != kInvalidNode) {
            scene.node(ids.model).transform.position = Vec3{0.0f, 2.0f, 0.0f};
        }
    } else {
        FUMAR_INFO("no model at '{}', showing the procedural scene only", modelPath.string());
    }

    FUMAR_INFO("scene: {} nodes, {} meshes, {} materials, {} textures", scene.nodeCount(),
               renderer.resources().meshCount(), renderer.resources().materialCount(),
               renderer.resources().textureCount());
    return ids;
}

/// Animates the scene for this frame. Pure transform edits - the renderer picks
/// them up on its next pass over the tree.
void animate(Scene& scene, const SandboxScene& ids, f32 elapsed) {
    const Vec3 up{0.0f, 1.0f, 0.0f};

    // Rotating the parent moves all three cubes in a circle without touching
    // their own transforms.
    scene.node(ids.pivot).transform.rotation = fromAxisAngle(up, elapsed * 0.4f);

    for (u32 i = 0; i < ids.cubes.size(); ++i) {
        // Each cube also spins in place, on top of the orbit it inherits.
        const f32 rate = 0.8f + static_cast<f32>(i) * 0.5f;
        scene.node(ids.cubes[i]).transform.rotation = fromAxisAngle(up, elapsed * rate);
    }

    if (ids.model != kInvalidNode) {
        scene.node(ids.model).transform.rotation = fromAxisAngle(up, elapsed * 0.25f);
    }
}

} // namespace

int main() {
    using Clock = std::chrono::steady_clock;

    FUMAR_INFO("fumar sandbox starting");

    Window window(WindowDesc{
        .title = "fumar - sandbox",
        .width = 1280,
        .height = 720,
        .resizable = true,
    });

    Renderer renderer(window);
    const SandboxScene ids = buildScene(renderer);

    renderer.camera().position = Vec3{0.0f, 3.0f, 9.0f};

    FUMAR_INFO("controls: hold right mouse to look, WASD to move, space/ctrl up and down,");
    FUMAR_INFO("          shift to sprint, escape to release the cursor or quit");

    const auto startTime = Clock::now();
    auto lastFrameTime = startTime;
    auto lastReport = startTime;
    u64 framesSinceReport = 0;

    while (!window.shouldClose()) {
        window.pumpEvents();

        // Nothing to draw while minimised, and a spin loop here would burn a
        // core for no reason. Blocking on the event queue keeps the window
        // responsive without the busy wait.
        if (window.minimized()) {
            window.waitEvents(100);
            // Reset the clock too, or the first frame after restoring would see
            // a delta of however long the window sat minimised and teleport the
            // camera across the scene.
            lastFrameTime = Clock::now();
            continue;
        }

        const auto now = Clock::now();
        const f32 elapsed = std::chrono::duration<f32>(now - startTime).count();

        // Clamped, because a breakpoint or a stalled frame would otherwise
        // produce one enormous step. Losing a little accuracy after a hitch
        // beats moving the camera a hundred metres in one frame.
        const f32 deltaSeconds = std::min(std::chrono::duration<f32>(now - lastFrameTime).count(), 0.1f);
        lastFrameTime = now;

        animate(renderer.scene(), ids, elapsed);
        renderer.camera().update(window, deltaSeconds);
        renderer.drawFrame();
        ++framesSinceReport;

        const auto sinceReport = std::chrono::duration<f64>(now - lastReport).count();
        if (sinceReport >= 1.0) {
            FUMAR_INFO("{:.0f} fps ({:.2f} ms/frame)", static_cast<f64>(framesSinceReport) / sinceReport,
                       1000.0 * sinceReport / static_cast<f64>(framesSinceReport));
            lastReport = now;
            framesSinceReport = 0;
        }
    }

    FUMAR_INFO("fumar sandbox shutting down");
    return 0;
}
