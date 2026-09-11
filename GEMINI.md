# Fumar Engine - Context & Project Memory

## 1. Project Overview & Philosophy
- **Name:** Fumar
- **Path:** `I:\code\PROJECTS\fumar`
- **Repository:** `https://github.com/c-airr/Fumar`
- **Goal:** Modern 3D graphics & game engine created to explore AI-assisted development and teach C++ architecture, linking, build systems, and graphics programming.
- **Tech Stack:**
  - **Language:** C++20 (C / ASM allowed where needed, e.g. `extern "C"`).
  - **Graphics API:** Pure Vulkan only (modern approach, Vulkan 1.3+ / 1.4 SDK, dynamic entry point resolution via SDL3 loader, no static `vulkan-1.lib` linking to support ARM64 & Linux seamlessly).
  - **Ray Tracing:** Hardware-accelerated Vulkan Ray Tracing KHR (`VK_KHR_ray_tracing_pipeline`, `VK_KHR_acceleration_structure`) for RT shadows, ambient occlusion, and reflections.
  - **Windowing & Input:** SDL3.
  - **Scripting:** Dual approach similar to Unreal Engine 5:
    - **LuaJIT:** Dynamic scripts running with zero-recompile hot-reloading (`lua/player.lua`, `lua/bob.lua`, etc.).
    - **C++ Scripting:** Dynamic module loading / hot-reloading for gameplay logic.
  - **UI / Editor:** ImGui + ImGuizmo, custom Unreal Engine 5 inspired styling.
  - **Language of Code & UI:** English only.

---

## 2. User Rules & Core Conventions
1. **Always commit and push after every change** (`claude.md`: *"zawsze commit z pushem po każdej zmianie"*). Keep commit messages clean and descriptive.
2. **Security & Credentials:**
   - Secrets and tokens belong in `.env` outside repositories, never in source files.
   - Remote machines are connected via Tailscale with Tailscale SSH; requesting machines hold no private keys.
   - Do NOT add remote build workers (`oraclearm`, `maslo`, etc.) without explicit user permission.
3. **Communication:**
   - Ask the user when facing design decisions or architectural choices; the user is learning C++ and understands the concepts well.
   - Practical, concise, direct communication.

---

## 3. UI / Editor Design & UX Guidelines
- **Theme:** Unreal Engine 5 aesthetic:
  - Dark background (lighter black / charcoal dark grey).
  - Accent color: `#A85F2C` (warm amber/orange-brown) applied on rounded borders of buttons, panels, active elements.
- **Layout:**
  - **Right:** Viewport (top-right / main), Outliner, and Details panel below Outliner (shows object details when selected, or World Details when nothing is selected).
  - **Top Bar:** Quick toolbar containing Play/Stop, Snapping options, and a **Place** popup button (dropdown to spawn Cube, Cylinder, Lighting, Empty, Player) to prevent UI clutter.
  - **Bottom:** Tabbed panels: Content Browser, Scripts, Statistics / Log.
- **Controls & Interaction:**
  - **Selection:** Raycast picking with mesh/edge highlight on hover, full selection on click.
  - **Gizmo:** ImGuizmo with Translation, Rotation, and Scaling.
  - **Shortcuts:**
    - `W`: Switch to Move / Translate tool.
    - `Ctrl + D`: Duplicate selected object.
    - `Ctrl + C` / `Ctrl + V`: Copy / Paste.
    - `Ctrl + Z`: Undo stack.
    - `Shift` (while scaling): Uniform proportional scale across all axes.
    - `RMB + WASD`: First-person flying camera in Viewport.
    - `Escape`: Release mouse capture / exit relative mouse mode.
  - **Cursor Behavior:**
    - Must lock and hide (SDL relative mouse mode) during viewport look and Play mode.
    - Must NOT flicker, drift, or appear as a stuck grey pointer over the viewport.
    - Activation click-through enabled (`SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH`).

---

## 4. Architecture & Directory Structure
- `engine/core`: Basic types, vector/matrix math, logging (`FUMAR_LOG`, `FUMAR_WARN`, `FUMAR_ERROR`).
- `engine/platform`: Window abstraction (`Window`), SDL3 wrapper, input handling, relative mouse mode.
- `engine/rhi`: Vulkan RHI (Instance, Device, Swapchain, VulkanBuffer, VulkanTexture, VulkanPipeline, Acceleration Structures).
- `engine/render`: Renderer, Camera, Ray tracing pipelines, PBR shading, Skybox, Post-processing / Tonemapping.
- `engine/scene`: Scene graph, Node transforms, Hierarchy, Lights (Directional & Point lights).
- `engine/script`: LuaJIT bindings (`fumar.key`, `fumar.mouse_delta`, `fumar.camera`, etc.) and C++ module runner.
- `engine/ui`: ImGui backend, ImGuizmo integration, custom theme & styling.
- `editor`: Main editor application (`editor/src/main.cpp`).
- `sandbox`: Lightweight sandbox app for isolated rendering tests.
- `shaders`: GLSL shaders compiled to SPIR-V via `glslc`:
  - `mesh.vert`, `mesh.frag`, `mesh_shading.glsl` (PBR forward/deferred passes)
  - `raytrace.glsl` (RT shadows, AO, reflections)
  - `sky.frag`, `sky.glsl`
  - `outline.frag`
  - `tonemap.frag`
- `scripts`:
  - `dev.ps1`: Windows development script (configures VS Clang-cl, Ninja, Vulkan SDK environment, runs CMake).
  - `linux-build.sh`: Linux build script with all apt dependencies and presets.
- `.github/workflows/release.yml`: Multi-target GitHub Actions build for `windows-x64`, `windows-arm64`, and `linux-x64`.

---

## 5. Build Commands & Presets
Run through `dev.ps1` on Windows:
- Configure Debug: `.\scripts\dev.ps1 cmake --preset windows-debug`
- Build Debug: `.\scripts\dev.ps1 cmake --build --preset windows-debug`
- Configure Release: `.\scripts\dev.ps1 cmake --preset windows-release`
- Build Release: `.\scripts\dev.ps1 cmake --build --preset windows-release`
- Run Editor: `.\build\windows-debug\bin\fumar_editor.exe`

---

## 6. Current Status & Pending Items (Handoff from Claude Code)
1. **Cursor Capture & Look Sensitivity (In Progress):**
   - User reported: *"jeszcze odrazu napraw kursor, teraz dalej nie jest zablokowany + wolno chodzi"*.
   - Uncommitted modifications exist in `editor/src/main.cpp`, `engine/platform/src/window.cpp`, `engine/platform/include/fumar/platform/window.hpp`.
   - Issue: `relativeMouse()` logic and ImGui SDL backend cursor visibility conflict; mouse look sensitivity is low (`0.12f` in camera & `player.lua`).
2. **GitHub Actions & Release:**
   - `.github/workflows/release.yml` is ready. Triggered on push tags `v*` (e.g. `git tag v0.1.0 && git push origin v0.1.0`) or manual workflow dispatch.
3. **Ray Tracing Artifacts:**
   - User noticed: light/shadow transition dots/noise that follow the screen rather than staying locked to world space geometry. Needs inspection of ray tracing jitter / sampling or screen-space reprojection.
4. **C++ Scripting & Asset Pipeline:**
   - Asset drag-and-drop / import was implemented.
   - C++ dynamic compilation / 3-second compile hot-reload workflow to be refined further.
