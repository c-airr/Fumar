#include "panels.hpp"

#include "fumar/core/log.hpp"
#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"
#include "fumar/platform/paths.hpp"
#include "fumar/platform/window.hpp"
#include "fumar/render/renderer.hpp"
#include "fumar/render/scene_io.hpp"
#include "fumar/scene/scene.hpp"
#include "fumar/native/native_engine.hpp"
#include "fumar/script/script_engine.hpp"
#include "fumar/ui/imgui_layer.hpp"

#include <ImGuizmo.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <utility>
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
    //
    // These are LINEAR reflectances, not the numbers a colour picker shows.
    // Something that looks like mid-grey on screen reflects about 20% of the
    // light hitting it, not 50 - the display's own curve accounts for the rest.
    // Feed 0.6 in here and the surface behaves like fresh snow, which is what
    // makes a scene look washed out no matter what the lighting does.
    const MaterialHandle floorMaterial =
        renderer.createMaterial("Floor", Vec4{0.17f, 0.175f, 0.185f, 1.0f});
    const MaterialHandle blockMaterial =
        renderer.createMaterial("Block", Vec4{0.32f, 0.325f, 0.34f, 1.0f});
    const MaterialHandle pillarMaterial =
        renderer.createMaterial("Pillar", Vec4{0.29f, 0.26f, 0.22f, 1.0f});

    // Roughness is what makes them read as different materials at all: the
    // floor scatters the sky evenly, the blocks hold a soft sheen, and the
    // pillar is smooth enough to show where the sun is.
    renderer.resources().material(floorMaterial).roughness = 0.85f;
    renderer.resources().material(blockMaterial).roughness = 0.55f;
    // Polished metal, so the scene shows a ray traced reflection without anyone
    // having to build one: the pillar picks up the floor, the blocks and the
    // sky. Turn its roughness up in Details and watch the reflection dissolve
    // into the sky gradient, which is the whole difference between the two.
    renderer.resources().material(pillarMaterial).roughness = 0.12f;
    renderer.resources().material(pillarMaterial).metallic = 1.0f;

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

    // A lamp, so the scene shows what a placed light does without anyone having
    // to add one first. Warm and close to the ground, where the sun does not
    // reach: a light that only brightens what is already lit teaches nothing.
    const NodeId lamp = scene.createNode("Lamp");
    scene.node(lamp).transform.position = Vec3{-2.6f, 1.5f, 2.2f};
    scene.node(lamp).light = Light{
        .color = Vec3{1.0f, 0.62f, 0.30f},
        .intensity = 55.0f,
        .range = 9.0f,
    };

    const std::filesystem::path modelPath = executableDirectory() / "assets" / "DamagedHelmet.glb";
    if (std::filesystem::exists(modelPath)) {
        const NodeId model = renderer.loadModel(modelPath);
        if (model != kInvalidNode) {
            scene.node(model).transform.position = Vec3{0.0f, 4.2f, 0.0f};

            // A C++ component, while a block above carries a Lua script. Both
            // run in the same frame off the same context, which is the whole
            // claim the dual scripting model makes - press Play and watch one
            // object driven from each side.
            scene.node(model).component = "Spinner";
        }
    }

    FUMAR_INFO("starter scene: {} nodes, {} meshes, {} materials", scene.nodeCount(),
               renderer.resources().meshCount(), renderer.resources().materialCount());
}

/// Copies the keyboard and mouse into the form scripts see them in.
///
/// A translation rather than a passthrough: fumar_script does not depend on the
/// platform layer, so it declares its own small set of keys and this is where
/// the two meet. Anything a script asks about that is not here simply reads as
/// not pressed.
void fillScriptInput(ScriptInput& input, const Window& window) {
    const auto set = [&](ScriptKey key, bool held) {
        input.held[static_cast<usize>(key)] = held;
    };

    set(ScriptKey::W, window.keyDown(Key::W));
    set(ScriptKey::A, window.keyDown(Key::A));
    set(ScriptKey::S, window.keyDown(Key::S));
    set(ScriptKey::D, window.keyDown(Key::D));
    set(ScriptKey::Q, window.keyDown(Key::Q));
    set(ScriptKey::E, window.keyDown(Key::E));
    set(ScriptKey::Space, window.keyDown(Key::Space));
    set(ScriptKey::Shift, window.keyDown(Key::LeftShift));
    set(ScriptKey::Control, window.keyDown(Key::LeftControl));
    set(ScriptKey::Up, window.keyDown(Key::Up));
    set(ScriptKey::Down, window.keyDown(Key::Down));
    set(ScriptKey::Left, window.keyDown(Key::Left));
    set(ScriptKey::Right, window.keyDown(Key::Right));
    set(ScriptKey::MouseLeft, window.mouseButtonDown(MouseButton::Left));
    set(ScriptKey::MouseRight, window.mouseButtonDown(MouseButton::Right));

    input.mouseDelta = window.mouseDelta();
}

/// Where Lua scripts are read from and written to.
///
/// In a development build this is the repository's lua/ directory, NOT the copy
/// made beside the executable - and the difference is the difference between
/// keeping your work and losing it. That copy exists so a distributed build is
/// self-contained; it is refreshed from the source on every build, so an edit
/// made there is overwritten the next time you compile, and it lives inside
/// build/, which is ignored by git and deleted outright by a clean.
///
/// C++ components already follow this rule - the Compile button builds
/// game/src, not a copy of it. Lua now does too.
std::filesystem::path luaScriptDirectory() {
#if defined(FUMAR_LUA_SOURCE_DIR)
    std::error_code ec;
    if (std::filesystem::exists(FUMAR_LUA_SOURCE_DIR, ec)) {
        return FUMAR_LUA_SOURCE_DIR;
    }
#endif
    // A release: there is no source tree, and the copy is the real thing. The
    // layout matches, so this is the same relative path either way.
    return executableDirectory() / "scripts" / "lua";
}

/// Keyboard shortcuts that apply when no text field has focus.
/// Keyboard shortcuts that apply when no text field has focus.
///
/// `navigating` is true while the camera is being flown. The tool shortcuts are
/// the letters WASD sit on top of, so without that check, holding right mouse
/// and pressing W to fly forward also swaps the gizmo to Move. Every editor
/// with a fly camera has to make this distinction; the mouse button is what
/// makes it.
void handleShortcuts(EditorState& state, bool navigating) {
    if (ImGui::GetIO().WantTextInput) {
        return;
    }

    if (!navigating) {
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
    }

    // Written as a modifier check plus a key press rather than with
    // IsKeyChordPressed: chords go through ImGui shortcut routing, which asks
    // which window owns the shortcut, and a global editor binding owned by no
    // particular panel is exactly the case that routing declines to deliver.
    const bool ctrl = ImGui::GetIO().KeyCtrl;
    const bool shift = ImGui::GetIO().KeyShift;

    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        // Ctrl+Shift+Z redoes as well as Ctrl+Y: both conventions are in wide
        // use and supporting one but not the other only ever annoys somebody.
        (shift ? state.redoRequested : state.undoRequested) = true;
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        state.redoRequested = true;
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        state.copyRequested = true;
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        state.pasteRequested = true;
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) {
        state.duplicateRequested = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        state.deleteRequested = true;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        state.selected = kInvalidNode;
    }
}

/// Carries out whatever the menu or the keyboard asked for.
///
/// Run between frames, never during one: undo and paste both destroy and create
/// nodes, and the panels have already been described from the tree as it was.
void applyEditActions(EditorState& state, Scene& scene) {
    const bool hasSelection = state.selected != kInvalidNode && scene.isAlive(state.selected);

    if (std::exchange(state.copyRequested, false) && hasSelection) {
        state.clipboard = captureSubtree(scene, state.selected);
        FUMAR_DEBUG("copied {} node(s)", state.clipboard.nodes.size());
    }

    if (std::exchange(state.pasteRequested, false) && !state.clipboard.empty()) {
        state.history.record(scene, state.selected);
        const NodeId pasted = pasteInto(scene, state.clipboard, kRootNode);
        if (pasted != kInvalidNode) {
            state.selected = pasted;
        }
    }

    // The copy lands in exactly the same place as the original and is selected
    // immediately, so the gizmo is already on it and it can be dragged off
    // without another click.
    if (std::exchange(state.duplicateRequested, false) && hasSelection) {
        state.history.record(scene, state.selected);
        const NodeId copy = scene.duplicateNode(state.selected);
        if (copy != kInvalidNode) {
            state.selected = copy;
        }
    }

    if (std::exchange(state.deleteRequested, false) && hasSelection) {
        state.history.record(scene, state.selected);
        scene.destroyNode(state.selected);
        state.selected = kInvalidNode;
    }

    NodeId restored = kInvalidNode;
    if (std::exchange(state.undoRequested, false) &&
        state.history.undo(scene, state.selected, restored)) {
        state.selected = restored;
    }

    restored = kInvalidNode;
    if (std::exchange(state.redoRequested, false) &&
        state.history.redo(scene, state.selected, restored)) {
        state.selected = restored;
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
    ScriptEngine scripts(luaScriptDirectory());
    scripts.compileAll();

    // The other half of the scripting model. Loaded rather than built at
    // startup: a rebuild takes seconds, and the library sitting beside the
    // executable is already current on a fresh build.
    NativeEngine native(executableDirectory());
    native.reload(renderer.scene());

    // The renderer records this after the scene, into the window - the scene
    // itself goes to an off-screen image that the viewport panel displays.
    renderer.setOverlay([&ui](vk::CommandBuffer cmd) { ui.record(cmd); });

    createStarterScene(renderer);
    renderer.camera().position = Vec3{7.5f, 5.5f, 10.0f};
    renderer.camera().yaw = -125.0f;
    renderer.camera().pitch = -20.0f;

    const std::filesystem::path sceneDirectory = executableDirectory() / "scenes";

    EditorState state;

    // Rebuilt each frame except for the camera, which a script may keep
    // adjusting across frames - so it lives out here rather than in the loop.
    ScriptContext scriptContext;
    state.sceneForRebuild = &renderer.scene();
    state.viewportTexture = ui.registerTexture(renderer.viewportImageView(), renderer.viewportSampler());

    // Survives across frames so the cursor can be captured BEFORE ImGui's
    // NewFrame - which is when the SDL backend would otherwise call
    // SDL_ShowCursor and undo relative mode. Known one frame late on the
    // first Play tick that takes the camera; that is preferable to fighting
    // the cursor every frame.
    bool scriptDrivesCamera = false;

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

        // --- cursor capture -------------------------------------------------
        // Decided and applied before NewFrame. ImGui's SDL backend updates the
        // OS cursor during NewFrame; if relative mode is still off at that
        // point it calls ShowCursor and the pointer comes back as a grey
        // unbound arrow instead of disappearing. Last frame's hover and script
        // ownership are enough: both already lag the UI by one frame elsewhere.
        const bool wantRelative =
            window.hasFocus() &&
            ((scriptDrivesCamera && state.scriptsRunning) ||
             (window.mouseButtonDown(MouseButton::Right) &&
              (window.relativeMouse() || state.viewportHovered)));

        if (wantRelative != window.relativeMouse()) {
            window.setRelativeMouse(wantRelative);
        }
        ui.setInputCaptured(window.relativeMouse());

        // --- interface ------------------------------------------------------
        ui.beginFrame();
        ImGuizmo::BeginFrame();

        drawDockspace(state, sceneDirectory);
        drawViewportPanel(state, renderer, scripts, native);
        drawOutlinerPanel(state, renderer);
        drawDetailsPanel(state, renderer.scene(), renderer, scripts, native);
        // The order here becomes the order of the tabs along the bottom.
        drawContentPanel(state, renderer);
        drawScriptsPanel(state, scripts, native);
        drawStatsPanel(state, renderer.scene(), renderer);

        // Which of them is in FRONT is a separate question, decided by whichever
        // was focused last - so on a fresh layout it would be Statistics purely
        // because it is drawn last. Asked for explicitly instead, and only for
        // the first few frames after the layout is built, so it never fights
        // the user clicking a different tab afterwards.
        if (state.focusContentFrames > 0) {
            --state.focusContentFrames;
            ImGui::SetWindowFocus("Content");
        }

        if (state.showImGuiDemo) {
            // Reachable from the Window menu: the fastest reference for what a
            // widget looks like and how it is called.
            ImGui::ShowDemoWindow(&state.showImGuiDemo);
        }

        // Held right mouse means the camera is being flown, which is what
        // stops WASD from doubling as the tool shortcuts.
        handleShortcuts(state, window.hasFocus() &&
                                   (window.mouseButtonDown(MouseButton::Right) ||
                                    window.relativeMouse()));

        // F5 recompiles, the way every editor with a build step does it.
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false) && !ImGui::GetIO().WantTextInput) {
            scripts.compileAll();
            native.rebuild(renderer.scene());
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
        scriptDrivesCamera = false;
        if (state.scriptsRunning) {
            fillScriptInput(scriptContext.input, window);

            // The camera goes in as it is and comes back out only if a script
            // wrote to it, so a scene where nothing wants the view leaves the
            // editor's own fly camera alone.
            scriptContext.camera.position = renderer.camera().position;
            scriptContext.camera.yaw = renderer.camera().yaw;
            scriptContext.camera.pitch = renderer.camera().pitch;
            scriptContext.camera.controlled = false;

            scriptContext.raycast = [&renderer](Vec3 origin, Vec3 rayDirection, f32 maxDistance) {
                return renderer.raycast(Ray{origin, rayDirection}, maxDistance);
            };

            scripts.update(renderer.scene(), scriptContext, deltaSeconds);

            // After the Lua pass and sharing its context, so a C++ component and
            // a Lua script on two different nodes see the same frame.
            native.update(renderer.scene(), scriptContext, deltaSeconds);

            if (scriptContext.camera.controlled) {
                scriptDrivesCamera = true;
                renderer.camera().position = scriptContext.camera.position;
                renderer.camera().yaw = scriptContext.camera.yaw;
                renderer.camera().pitch = scriptContext.camera.pitch;

                // Same-frame grab for the first Play tick that claims the
                // camera - next frame the pre-NewFrame path keeps it. Escape
                // still releases via Window; the next frame then sees
                // relativeMouse false until the script path asks again, which
                // it does while focused so Play stays locked until Stop.
                if (window.hasFocus() && !window.relativeMouse()) {
                    window.setRelativeMouse(true);
                    ui.setInputCaptured(true);
                }
            }
        }

        // --- camera ---------------------------------------------------------
        // Flying is allowed only from inside the viewport, so dragging in a
        // panel never moves the view. Once the cursor is captured the check is
        // skipped, because in that mode ImGui no longer receives meaningful
        // positions and would report the cursor as being nowhere.
        // hasFocus first, and it is not a detail: the OS reports the physical
        // state of the mouse whichever window is in front, so without it the
        // camera flies around while you are clicking in a browser behind it.
        // Not while a script owns the view: two things writing the camera in
        // the same frame means whichever runs last wins, which looks like the
        // controls fighting each other.
        // Capture itself is owned above (wantRelative); Camera::update only
        // applies look/move while active and must not fight that decision.
        const bool cameraActive =
            !scriptDrivesCamera && window.hasFocus() &&
            (window.relativeMouse() ||
             (state.viewportHovered && window.mouseButtonDown(MouseButton::Right)));

        if (cameraActive) {
            renderer.camera().update(window, deltaSeconds);
        }

        updatePicking(state, renderer, cameraActive);
        renderer.setSelected(state.selected);

        ui.endFrame();
        renderer.drawFrame();

        // --- edits ------------------------------------------------------------
        // Between frames, for the same reason the file actions below are:
        // undo and paste destroy and create nodes, and the panels have already
        // described the tree as it was.
        applyEditActions(state, renderer.scene());

        // --- dropped files ----------------------------------------------------
        // Handled after the frame for the same reason the file actions are:
        // importing adds nodes, and the panels have already been described.
        for (const std::string& dropped : window.consumeDroppedFiles()) {
            const std::filesystem::path path(dropped);
            if (!Renderer::isImportable(path)) {
                FUMAR_WARN("ignoring dropped file '{}': unsupported type", path.filename().string());
                continue;
            }
            const NodeId imported = renderer.importAsset(path);
            if (imported != kInvalidNode) {
                state.selected = imported;
            }
        }

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

                // Both hold mesh and material handles, which are indices into
                // a registry the load has just replaced. Undoing into the old
                // scene would rebuild it with whatever now sits at those
                // indices - a different mesh, or none.
                state.history.clear();
                state.clipboard = SceneSnapshot{};

                scripts.restart();
            native.restart();
                native.restart();
            }
        }

        if (state.newSceneRequested) {
            state.newSceneRequested = false;
            renderer.resetScene();
            createStarterScene(renderer);
            state.scenePath.clear();
            state.selected = kInvalidNode;
            state.hovered = kInvalidNode;
            state.history.clear();
            state.clipboard = SceneSnapshot{};
            scripts.restart();
            native.restart();
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
