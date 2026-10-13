# PBR Materials Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Materials in fumar carry base colour, metallic-roughness, normal, occlusion and emissive maps plus an alpha cutout. They come from glTF, from dropped texture-set folders and from a stock library, and can be edited live in the Modeler's Materials panel and saved as `.fmat` files.

**Architecture:**
- One per-frame storage buffer of `MaterialRecord`s and one 1024-slot texture array, shared by the rasteriser and the ray queries.
- Draws and ray instances carry only a material index.
- Edits are written per frame in flight after that frame's fence, never with `waitIdle`.
- Tangents live in `Vertex` (48 bytes). They come from glTF or MikkTSpace for imports, and from UV gradients for meshes edited in the Modeler.

**Tech Stack:** C++20, Vulkan 1.3 (vulkan-hpp, VMA), GLSL 460 (ray query, buffer_reference, scalar layout), cgltf, stb_image, nlohmann_json, ImGui/ImGuizmo, SDL3, MikkTSpace (new, FetchContent).

**Spec:** `docs/superpowers/specs/2026-10-14-pbr-materials-design.md`. Read it before any task. Section numbers below (§1.2 etc.) refer to it.

## Global Constraints

- **Language:** all code, comments, logs and file formats in English. Docs get an English file and a Polish `*.pl.md` twin.
- **Platform:** Vulkan only, Windows and Linux. Nothing in `fumar_geometry` or the test executables may need a GPU.
- **Building:** `& I:\code\PROJECTS\fumar\tools\dev.ps1 cmake --build --preset windows-debug` (and `windows-release`), from PowerShell. `cmake` is not on the Bash PATH.
- **Validation:** layers exist only in the debug build. Every renderer change is run in debug with zero `VUID` lines before it is called done.
- **Committing:** after every task, `git pull --ff-only`, commit, push to `main`. Message trailer:
  - `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
  - `Claude-Session: https://claude.ai/code/session_01W8BjfqaDfo7Ps3gJ6Ec61J`
- **Never:** write `README.md`; simulate keyboard input (drive the editor through `FUMAR_MODELER_SMOKE`); kill an editor process the user started.
- **Fixed sizes:**
  - `Vertex` is 48 bytes: position, normal, uv, `Vec4 tangent`, with `w` = bitangent sign ±1.
  - `MaterialRecord` is 64 bytes, scalar layout. Initial capacity 256 records per frame in flight; grows by doubling.
  - `kMaxSceneTextures = 1024`.
  - Push constants: `mat4 model`, `vec4 overlayColor`, `uint materialIndex`, `float highlight`, 88 bytes in total.
- **Colour space:** base colour and emissive are sRGB. Metal-rough, normal and occlusion are linear.
- **Neutral textures:** white for base colour, metal-rough and occlusion. `(128,128,255,255)` for normal. Black for emissive.
- **Drag cost:** the `helmet` smoke's "re-uploaded in X ms per frame of a drag" must stay at most 2.6 ms in release. It is 2.2 ms today.
- **Smoke runner:** `scratchpad\smoke.ps1 -Variant <v> [-Preset windows-release] [-Seconds N]`. It starts the editor with `FUMAR_MODELER_SMOKE=<v>`, waits, stops only that process, and prints its log.

## Review Focus

The five inputs the spec implies but does not test, most likely to bite first:

1. **More than 1024 textures in the scene.** The extra textures must sample the neutral texture and log one warning, not index out of the array. Tested in Task 3: `textureSlot()`.
2. **A `.fmat` whose texture file is missing or unreadable.** The material must still load, that slot gets the neutral texture, and one warning names the file. Tested in Task 5, smoke step `materials`/missing-map.
3. **A malformed `.fmat`:** not JSON, wrong types, or `"fumarMaterial": 2`. It must return `std::nullopt` with a logged reason, never throw or crash. Tested in Task 5: `material_tests` "malformed".
4. **A dropped folder with no recognisable base-colour or normal map** (screenshots, a zip). No material is made, and one warning is logged. Tested in Task 5: `findTextureSet` on such a list returns `!usable()`.
5. **A scene that references a `.fmat` that has since been moved or deleted.** The scene must load, the nodes get the fallback material, and one warning is logged. Tested in Task 5, smoke step `materials`/dangling-fmat.

---

## File map

| File | Responsibility | Task |
|---|---|---|
| `engine/geometry/include/fumar/geometry/editable_mesh.hpp`, `src/editable_mesh.cpp` | `TriangleMesh::tangents`, emitted by `triangulate()` | 1 |
| `cmake/FumarDependencies.cmake` | MikkTSpace FetchContent + `fumar_mikktspace` static lib | 2 |
| `engine/render/include/fumar/render/tangents.hpp`, `src/tangents.cpp` (new) | `computeTangents(span<Vertex>, span<const u32>)` via MikkTSpace | 2 |
| `engine/render/include/fumar/render/mesh.hpp`, `src/mesh.cpp` | `Vertex::tangent`, attribute 3, `makeMesh` copies tangents | 2 |
| `engine/render/src/gltf_loader.cpp`, `src/gltf_writer.cpp` | read and write `TANGENT` | 2 |
| `shaders/mesh.vert`, `shaders/raytrace.glsl` | vertex input and RT vertex struct gain the tangent | 2 |
| `tools/material_tests/` (new) | headless tests for tangents, texture sets, `.fmat`, packing, slots | 2, 3, 5 |
| `engine/render/include/fumar/render/resources.hpp` | new `Material`, `AlphaMode`, `MaterialOrigin` | 3 |
| `engine/render/src/material_gpu.hpp/.cpp` (new) | `MaterialRecord`, `toRecord()`, `textureSlot()`, `MaterialTable` (per-frame buffer + dirty tracking) | 3 |
| `engine/render/src/renderer.cpp`, `include/fumar/render/renderer.hpp` | set 0 binding 4, 1024-slot array on all GPUs, per-frame descriptor writes, draw loop, instance records | 3 |
| `shaders/object.glsl`, `shaders/outline.frag` | new push-constant block; outline drops set 1 | 3 |
| `shaders/material.glsl` (new), `mesh_shading.glsl`, `mesh.frag`, `mesh_rq.frag`, `raytrace.glsl` | material sampling, normal map, AO, emissive, cutout, emissive in GI | 4 |
| `engine/render/src/gltf_loader.cpp` | all glTF material inputs, per-use colour space, lazy textures | 4 |
| `engine/render/include/fumar/render/material_file.hpp`, `src/material_file.cpp` (new) | `MaterialDesc`, `readMaterialFile`, `writeMaterialFile` | 5 |
| `engine/render/include/fumar/render/texture_set.hpp`, `src/texture_set.cpp` (new) | `findTextureSet`, `packMetallicRoughness` | 5 |
| `engine/render/src/renderer.cpp`, `src/scene_io.cpp` | `createMaterial(const MaterialDesc&)`, `loadMaterialFile`, folder import, `.fmat` refs in scenes | 5 |
| `editor/src/history.hpp/.cpp` | material parameters in `SceneSnapshot` | 6 |
| `engine/platform/include/fumar/platform/file_dialog.hpp`, `src/file_dialog.cpp` (new) | SDL3 open-file dialog, polled from the UI thread | 7 |
| `editor/src/materials_panel.hpp/.cpp` (new) | the Materials panel, moved out of `modeler.cpp` and rewritten | 7 |
| `editor/src/modeler.cpp`, `editor/src/main.cpp` | remove the old panel; Ctrl+S saves modified `.fmat`s; smoke `materials` | 7 |
| `tools/get-materials.ps1`, `tools/get-materials.sh`, `docs/materials.md`, `docs/materials.pl.md` (new); branch `materials` | stock library | 8 |
| `editor/src/main.cpp` (`createStarterScene`) | stock materials when present | 8 |
| `.github/workflows/release.yml` | run `material_tests` | 2 |

---

### Task 1: Tangents from `triangulate()`

**Files:**
- Modify: `engine/geometry/include/fumar/geometry/editable_mesh.hpp` (`TriangleMesh`), `engine/geometry/src/editable_mesh.cpp` (`EditableMesh::triangulate`)
- Test: `tools/geometry_tests/src/main.cpp`

**Interfaces:**
- Produces: `std::vector<Vec4> TriangleMesh::tangents`, one per corner, the same size as `positions`. `xyz` is unit length and orthogonal to that corner's normal; `w` is +1 or -1.

- [ ] **Step 1: Write the failing test** `tangents()` in `geometry_tests` (call it from `main` after `hostileInput()`):
  - For `makeCube()`, `makePlane(1.0f)` and `makeUvSphere(1.0f)`, `triangulate()` gives `tangents.size() == positions.size()`.
  - Every tangent: `|length(xyz) - 1| < 1e-3`, `|dot(xyz, normal)| < 1e-3`, `w == 1 || w == -1`.
  - The plane's top face with default UVs: tangent ≈ `+X` (`dot > 0.99`), and the bitangent `cross(n, t) * w` ≈ the direction of +V.
  - A degenerate-UV quad (all four corners at UV (0,0)): every tangent is finite and satisfies the unit and orthogonality checks.
- [ ] **Step 2: Run it and confirm it fails.** `build\windows-debug\bin\geometry_tests.exe`: does not compile (`tangents` missing). Add the member, then it runs with FAIL lines.
- [ ] **Step 3: Implement it in `triangulate()`.**
  - Per face, the tangent comes from the UV gradient of its fan's first triangle, using the standard `(dP1 * dV2 - dP2 * dV1) / det` formula. The handedness is `w = sign(dot(cross(n, t), bitangent))`.
  - Per corner, average the tangents of the same faces whose normals were averaged (the CSR `facesAt` walk already there, same `smoothCos` test). Then Gram-Schmidt against the corner's normal and normalise.
  - With `|det| < 1e-12`, or a result shorter than 1e-6, use `normalize(cross(n, |n.y| < 0.99 ? +Y : +X))`, `w = +1`.
  - Reserve `tangents` with the other outputs.
- [ ] **Step 4: Run the tests and confirm they pass.** `geometry_tests.exe` exits 0, prints "all good", and the new checks show PASS.
- [ ] **Step 5: Check the drag cost has not regressed.**
  - Build release.
  - Run `smoke.ps1 -Variant helmet -Preset windows-release` twice.
  - Expected: "re-uploaded in ≤ 2.6 ms". Tangents are not uploaded yet, so this measures only the CPU side.
- [ ] **Step 6: Commit.** "Emit tangents from triangulate()"

### Task 2: `Vertex.tangent`, MikkTSpace, glTF tangents, `material_tests`

**Files:**
- Modify: `cmake/FumarDependencies.cmake`, `engine/render/CMakeLists.txt`, `engine/render/include/fumar/render/mesh.hpp`, `engine/render/src/mesh.cpp`, `engine/render/src/gltf_loader.cpp`, `engine/render/src/gltf_writer.cpp`, `shaders/mesh.vert`, `shaders/raytrace.glsl`, `CMakeLists.txt` (add `tools/material_tests`), `.github/workflows/release.yml`
- Create: `engine/render/include/fumar/render/tangents.hpp`, `engine/render/src/tangents.cpp`, `tools/material_tests/CMakeLists.txt`, `tools/material_tests/src/main.cpp`

**Interfaces:**
- Consumes: `TriangleMesh::tangents` (Task 1).
- Produces:
  - `struct Vertex { Vec3 position; Vec3 normal; Vec2 uv; Vec4 tangent; };` with `static_assert(sizeof(Vertex) == 48)`.
  - `void computeTangents(std::span<Vertex> vertices, std::span<const u32> indices);` in `fumar/render/tangents.hpp`. It overwrites `tangent` for every vertex an index references.
  - Executable `material_tests`, in the same style as `geometry_tests`: `check(bool, std::string)`, prints PASS/FAIL, exits 0 or 1.

- [ ] **Step 1: Add MikkTSpace and the `material_tests` skeleton.**
  - In `FumarDependencies.cmake`:
    - `FetchContent_Declare(mikktspace GIT_REPOSITORY https://github.com/mmikk/MikkTSpace.git GIT_TAG <current master commit hash>)`, with a comment saying it is pinned to a commit because there are no tags.
    - `add_library(fumar_mikktspace STATIC ${mikktspace_SOURCE_DIR}/mikktspace.c)` with its include dir, and warnings off for it as other third-party targets do.
  - `fumar_render` links it `PRIVATE`.
  - `tools/material_tests` links `fumar::render` and `fumar::geometry`, and is added beside `geometry_tests`.
  - Add a "Test materials" step after "Test geometry" in both jobs of `release.yml` (the Windows and Linux paths, as for `geometry_tests`).
- [ ] **Step 2: Write the failing test** in `material_tests`:
  - `mikkTangentsOnQuad`: two triangles, positions (0,0,0), (1,0,0), (1,0,1), (0,0,1), normal +Y, UVs (0,0), (1,0), (1,1), (0,1). After `computeTangents`, every tangent is `(1,0,0)` within 1e-4 and `w = -1`, because V runs along +Z while the normal is +Y. Assert the value MikkTSpace actually gives, and comment why.
  - `mikkTangentsOrthonormal`: on a 16×8 UV sphere built through `geometry::makeUvSphere(1.0f, 16, 8)` and `triangulate()`, after `computeTangents` every tangent is unit length, orthogonal to its normal within 1e-3, and has `w` = ±1.
- [ ] **Step 3: Run it and confirm it fails.** `material_tests.exe` does not link (`computeTangents` missing).
- [ ] **Step 4: Implement `computeTangents`** with the `SMikkTSpaceInterface` callbacks over the index list (faces = `indices.size() / 3`), writing through `m_setTSpaceBasic`.
- [ ] **Step 5: Thread the tangents through.**
  - `Vertex` gains `tangent`; `Vertex::attributes()` returns 4 entries, location 3 `eR32G32B32A32Sfloat`.
  - `makeMesh` copies `TriangleMesh::tangents`.
  - `gltf_loader` `buildMesh` reads `cgltf_attribute_type_tangent` (`readAttribute<4>`). Without it, it calls `computeTangents` after normals exist (after `generateNormals` when that ran).
  - `gltf_writer` writes a `TANGENT` accessor (`VEC4`, float) and adds it to the primitive's attributes.
  - `mesh.vert`: `layout(location = 3) in vec4 inTangent;`, output `vTangent` (world space, `mat3(object.model) * inTangent.xyz`, w passed through). The fragment shaders that do not use it yet still compile.
  - `raytrace.glsl`: the `buffer_reference` vertex struct gains `vec4 tangent` after `uv`, so its stride is 48. Comment that it must match `Vertex`.
  - `Mesh::toEditable` and every other `Vertex{...}` brace-init site gets `tangent` (grep `Vertex{`).
- [ ] **Step 6: Run the tests.**
  - `material_tests.exe` and `geometry_tests.exe` both exit 0.
  - Debug smoke `finish`, `save` and `helmet` show no VUIDs and the same lines as before.
  - `save` still reports "18 polygon sizes for 18 faces".
- [ ] **Step 7: Check the picture is unchanged.**
  - Release build, start the editor with the starter scene, screenshot.
  - It is identical to before. Ray-traced reflections still read the right UVs: if the RT vertex stride were wrong, reflected textures would scramble.
  - Re-run the `helmet` release smoke and confirm ≤ 2.6 ms.
- [ ] **Step 8: Commit.** "Give vertices tangents: MikkTSpace for imports, UV gradients for edited meshes"

### Task 3: Material buffer, 1024-slot texture array, material index in draws and rays

The result looks exactly as it does today; only the plumbing changes.

**Files:**
- Modify:
  - `engine/render/include/fumar/render/resources.hpp`
  - `engine/render/include/fumar/render/renderer.hpp`
  - `engine/render/src/renderer.cpp`
  - `engine/render/src/gltf_loader.cpp`
  - `engine/render/include/fumar/render/gltf_loader.hpp` (`GltfLoadContext` loses `descriptorPool` / `materialSetLayout`)
  - `shaders/object.glsl`, `shaders/mesh_shading.glsl`, `shaders/raytrace.glsl`, `shaders/outline.frag`, `shaders/sky.frag` (if it binds set 1)
  - `engine/render/src/scene_io.cpp` (uses the old material fields)
- Create: `engine/render/src/material_gpu.hpp`, `engine/render/src/material_gpu.cpp`
- Test: `tools/material_tests/src/main.cpp`

**Interfaces:**
- Produces:
  - `resources.hpp`: `enum class AlphaMode : u8 { Opaque, Mask };`
  - `struct MaterialOrigin { enum class Kind : u8 { Builtin, Gltf, Fmat } kind = Kind::Builtin; std::string file; u32 index = 0; };`. The file is a glTF path, a `.fmat` path, or for `Builtin` the single base-colour texture path, which replaces `baseColorPath`.
  - `Material` exactly as spec §1.1, without `descriptorSet`, `sourceFile`, `sourceIndex` or `baseColorPath`. Each texture slot also keeps `std::string` source paths for `.fmat` writing: `baseColorPath`, `metallicPath`, `roughnessPath`, `packedMetallicRoughnessPath`, `normalPath`, `normalIsDirectX`, `occlusionPath`, `emissivePath`.
  - `material_gpu.hpp`:
    - `struct MaterialRecord` (spec §1.2, `static_assert(sizeof == 64)`);
    - `u32 textureSlot(TextureHandle handle, usize textureCount, u32 neutral);` returns `handle.index` when it is valid, below `textureCount` and below `kMaxSceneTextures`, and `neutral` otherwise;
    - `MaterialRecord toRecord(const Material&, const NeutralTextures&, usize textureCount);`
    - `struct NeutralTextures { u32 white, flatNormal, black; };` holds the slots of the three neutral textures.
  - `Renderer::markMaterialDirty(MaterialHandle)`, public, used by the editor in Tasks 6 and 7.
  - The draw loop pushes `{model, overlayColor, materialIndex, highlight}`. `InstanceRecord` is `{u64 vertices; u64 indices; u32 materialIndex; u32 padding;}`, mirrored in `raytrace.glsl`.

- [ ] **Step 1: Write the failing tests** in `material_tests`:
  - `textureSlots`: `textureSlot({5}, 10, 0) == 5`; `textureSlot({}, 10, 7) == 7`; `textureSlot({12}, 10, 7) == 7`; `textureSlot({1500}, 2000, 7) == 7`. The last one is Review Focus #1.
  - `recordFromMaterial`:
    - A `Material` with `emissiveColor (1, 0.5, 0)` and `emissiveIntensity 4` gives `record.emissive == (4, 2, 0)`.
    - `alphaMode Opaque` gives `alphaCutoff < 0`; `Mask` with cutoff 0.3 gives 0.3.
    - An empty `normal` slot gives `normalTexture == neutral.flatNormal`.
- [ ] **Step 2: Run it and confirm it fails.** It does not compile.
- [ ] **Step 3: Implement `material_gpu.cpp`.** Pure functions, no Vulkan calls, so the test links without a device.
- [ ] **Step 4: Rebuild the renderer around them.**
  - **Neutral textures.** Created at startup: 1×1 white (sRGB), 1×1 `(128,128,255,255)` linear, 1×1 black sRGB. They are registered through `m_resources.addTexture` so they own the first three slots, and `NeutralTextures` records those slots.
  - **Set 0 layout**, on all GPUs:
    - binding 3 is `kMaxSceneTextures` combined image samplers;
    - binding 4 is a storage buffer of `MaterialRecord` (vertex and fragment stages);
    - bindings 1 and 2 stay RT-only.
    - Pool sizes change to match.
    - Remove `kMaterialBudget` and the per-material sets.
    - `m_materialSetLayout` stays, renamed `m_imageSetLayout`, for the bloom and fallback uses that borrowed it.
  - **`PerFrame`** gains `rhi::Buffer materialData; u32 materialCapacity; std::vector<bool> materialDirty; u32 texturesWritten;`.
  - **At frame start**, after `waitForFrameSlot()`, replacing `refreshTextureArray`:
    - grow `materialData` when `materialCount > capacity` (double it; the slot's fence has been waited, so free and rewrite binding 4);
    - write the dirty records;
    - write texture slots `[texturesWritten, textureCount)` into this frame's `cameraSet` binding 3, then set `texturesWritten`.
    - Slots `≥ textureCount` are filled with white once, when the set is allocated.
    - When `textureCount > kMaxSceneTextures`, log one warning ("texture N and above are not reachable; they sample white").
  - **`addMaterial`, `createMaterial` and the glTF loader** call `markMaterialDirty` (all frames). Replacing the resources (`resetScene`, scene load) resets `texturesWritten` to 0 and marks every material dirty.
  - **Draw loop:** no `bindDescriptorSets(..., 1, material...)`. Push `materialIndex = handle.index`. `overlayColor` is unused by the mesh pipeline.
  - **Outline and grid pipelines:** set 1 goes from their layouts, and the outline keeps `overlayColor`.
  - **Instance records:** `materialIndex` only.
  - **Shaders:**
    - `object.glsl`: the new block, with the offsets commented.
    - `mesh_shading.glsl` and `raytrace.glsl` read `materials[object.materialIndex]` / `materials[record.materialIndex]`. They declare the storage buffer at set 0 binding 4 and sample `sceneTextures[...]` with the record's `baseColorTexture`. In the rasteriser that index is uniform per draw, so no `nonuniformEXT`; in hits it is non-uniform.
    - The non-RT `mesh.frag` now also declares binding 3 and binding 4.
  - **`scene_io`** saves and loads through `MaterialOrigin` and the path fields. It stays format-compatible: `"texture"` maps to `Builtin`, `"file"` + `"index"` maps to `Gltf`.
- [ ] **Step 5: Run the tests.**
  - `material_tests` exits 0.
  - Debug smoke `finish`, `save`, `helmet`: no VUIDs, same lines.
  - Release screenshot of the starter scene: same as Task 2's, pixel for pixel apart from temporal noise.
- [ ] **Step 6: Check edits apply live.**
  - Temporary: in a debug-only smoke step, set Block A's material roughness to 0.05, `markMaterialDirty`, and screenshot two frames later. The block turns glossy.
  - Remove that temporary step before committing; Task 7's smoke covers it for good.
- [ ] **Step 7: Commit.** "Materials in one buffer, textures in one array, for the rasteriser and the rays alike"

### Task 4: The five maps in the shaders, and glTF materials in full

**Files:**
- Create: `shaders/material.glsl`
- Modify: `shaders/mesh_shading.glsl`, `shaders/mesh.frag`, `shaders/mesh_rq.frag`, `shaders/raytrace.glsl`, `engine/render/src/gltf_loader.cpp`, `engine/render/src/material_gpu.cpp` (if a field is missing), `docs/scripting.md` / `.pl.md` only if they mention materials (they do not today)

**Interfaces:**
- Consumes: `MaterialRecord`, binding 3 and binding 4 (Task 3), `vTangent` (Task 2).
- Produces (GLSL, spec §1.4): `MaterialSample sampleMaterial(uint index, vec2 uv, vec3 n, vec4 tangent)` and `MaterialSample sampleMaterialHit(uint index, vec2 uv, vec3 n)`.

- [ ] **Step 1: Write `material.glsl`.**
  - The `MaterialRecord` struct and the binding-4 buffer move here from Task 3's copies, so there is one definition.
  - `sampleMaterial`:
    - TBN from `n`, `tangent.xyz` and `cross(n, t) * tangent.w`;
    - normal map `(tex * 2 - 1)` with `xy *= normalScale`;
    - roughness = `factor * tex.g`, metallic = `factor * tex.b`;
    - occlusion = `mix(1, tex.r, occlusionStrength)`;
    - emissive = `record.emissive * tex.rgb`;
    - alpha = `baseColorFactor.a * tex.a`.
  - `sampleMaterialHit` uses `textureLod(..., 0)` and `nonuniformEXT`, with no normal map. Keep the existing comment about LOD 0.
- [ ] **Step 2: Shade with it.**
  - `mesh_shading.glsl` calls `sampleMaterial`.
  - Occlusion multiplies only the ambient and indirect terms.
  - Emissive is added after lighting, before the highlight tint.
  - `if (record.alphaCutoff >= 0 && sample.alpha < record.alphaCutoff) discard;`
  - The `raytrace.glsl` hit shading uses `sampleMaterialHit` for albedo, roughness, metallic and emissive.
  - `indirectBounce` adds the hit's `emissive` to the radiance it returns, before the hit's own shadow-ray lighting, so a glowing surface lights its surroundings.
- [ ] **Step 3: Load glTF materials in full** (spec §2.1).
  - All inputs: base colour, metal-rough, normal + scale, occlusion + strength, emissive + factor + `KHR_materials_emissive_strength` (cgltf: `has_emissive_strength`), `alphaMode` / `alphaCutoff`. `BLEND` is loaded as opaque, with `FUMAR_DEBUG`.
  - Textures are created lazily, per (image, colour space) pair: a small map from `(imageIndex, srgb)` to `TextureHandle`. Only referenced images are uploaded, and base colour/emissive use sRGB while the others are linear.
  - `MaterialOrigin{Gltf, path, index}`.
- [ ] **Step 4: Verify on the helmet.**
  - Debug smoke `helmet`: no VUIDs.
  - Release screenshot of the starter scene, from the camera that frames the helmet. Compare with Task 3's: normal-map detail and the emissive visor glow are now visible, and the metal parts differ from the rubber.
  - 4× nearest-neighbour crop of the visor and of a panel seam, as evidence.
- [ ] **Step 5: Check emission lights its surroundings.**
  - Temporary: give Block B an emissive colour of (1, 0.4, 0.1) at intensity 8 in a debug-only smoke step.
  - Screenshot: the floor next to it picks up orange light through GI.
  - Remove the step before committing.
- [ ] **Step 6: Run the regression tests.** `geometry_tests` and `material_tests` exit 0; smoke `finish`, `save` and `orbit` show no VUIDs.
- [ ] **Step 7: Commit.** "Shade with all five glTF maps, and let emissive surfaces light the scene"

### Task 5: `.fmat` files, texture-set folders, and `.fmat` in scenes

**Files:**
- Create: `engine/render/include/fumar/render/material_file.hpp`, `engine/render/src/material_file.cpp`, `engine/render/include/fumar/render/texture_set.hpp`, `engine/render/src/texture_set.cpp`
- Modify: `engine/render/include/fumar/render/renderer.hpp`, `engine/render/src/renderer.cpp`, `engine/render/src/scene_io.cpp`, `editor/src/main.cpp` (folder drops)
- Test: `tools/material_tests/src/main.cpp`

**Interfaces:**
- Produces:
  - `material_file.hpp`:
    ```cpp
    struct MaterialDesc {
        std::string name;
        std::filesystem::path baseColor, packedMetallicRoughness, metallic, roughness,
                              normal, occlusion, emissive;   // absolute; empty = none
        bool normalIsDirectX = false;
        Vec4 baseColorFactor{1, 1, 1, 1};
        f32 metallicFactor = 0, roughnessFactor = 1, normalScale = 1, occlusionStrength = 1;
        Vec3 emissiveColor{0, 0, 0}; f32 emissiveIntensity = 1;
        AlphaMode alphaMode = AlphaMode::Opaque; f32 alphaCutoff = 0.5f;
        Vec2 uvScale{1, 1};
    };
    std::optional<MaterialDesc> readMaterialFile(const std::filesystem::path& file);
    bool writeMaterialFile(const std::filesystem::path& file, const MaterialDesc& desc);
    MaterialDesc describeMaterial(const Material& material);   // for saving an edited one
    ```
    The JSON shape is spec §3.1. Paths are stored relative to the `.fmat` file and resolved to absolute paths on read. `writeMaterialFile` writes `<file>.partial` and renames it over the target.
  - `texture_set.hpp`:
    ```cpp
    struct TextureSetFiles {
        std::filesystem::path baseColor, normal, roughness, metallic, occlusion, emissive;
        bool normalIsDirectX = false;
        bool usable() const;   // base colour or normal present
    };
    TextureSetFiles findTextureSet(std::span<const std::filesystem::path> files);
    TextureSetFiles findTextureSet(const std::filesystem::path& folder);   // lists, then calls the above
    struct Pixels { u32 width = 0, height = 0; std::vector<u8> rgba; };
    Pixels packMetallicRoughness(const Pixels* roughness, const Pixels* metallic);
    ```
    `packMetallicRoughness` takes R of each input: G = roughness R, B = metallic R; R = 255, A = 255. A missing input gives 255. The output size is the larger of the two, with the smaller resampled bilinearly. If both are null, the result is 1×1.
  - `Renderer`:
    - `MaterialHandle createMaterial(const MaterialDesc& desc, MaterialOrigin origin);`
    - `MaterialHandle loadMaterialFile(const std::filesystem::path& file);` returns an invalid handle on failure. The same path loaded twice returns the same handle (map by `weakly_canonical` path).
    - `bool saveMaterialFile(MaterialHandle);` writes `describeMaterial` to `origin.file` for `Fmat` origins.
    - `importAsset` accepts a directory: `findTextureSet`; if `usable()`, write `<folder>/<folder name>.fmat` and `loadMaterialFile` it. `isImportable` returns true for directories.
  - The old `createMaterial(name, colour, texturePath)` stays as a thin wrapper building a `MaterialDesc` with `Builtin` origin.

- [ ] **Step 1: Write the failing tests** in `material_tests`:
  - **`textureSetAmbientCg`:** the files `Bricks059_2K-JPG_Color.jpg`, `_NormalGL.jpg`, `_NormalDX.jpg`, `_Roughness.jpg`, `_AmbientOcclusion.jpg`, `_Displacement.jpg`, and `Bricks059.png` (a preview image) give: baseColor = Color, normal = NormalGL with `normalIsDirectX == false`, roughness and occlusion set, metallic empty. Displacement and the preview are ignored.
  - **`textureSetPolyHaven`:** `brick_wall_diff_2k.jpg`, `brick_wall_nor_dx_2k.jpg`, `brick_wall_rough_2k.jpg`, `brick_wall_ao_2k.jpg` give normal = nor_dx with `normalIsDirectX == true`. The suffix may be followed by `_2k`; matching looks for the suffix token anywhere after the stem's first `_`, case-insensitive.
  - **`textureSetMixedCase`:** `WOOD_COLOR.PNG` and `wood_normal.png` are recognised.
  - **`textureSetUnusable`** (Review Focus #4): `screenshot.png` and `notes.txt` give `!usable()`.
  - **`packingSizes`:** a 4×4 roughness image of value 100 and a 2×2 metallic image of value 200 give 4×4 output, every pixel `(255, 100, 200, 255)`. Roughness only gives B = 255.
  - **`fmatRoundTrip`:** a `MaterialDesc` with every field non-default, written to a temp dir and read back, compares equal field by field. Paths come back absolute and equal.
  - **`fmatMalformed`** (Review Focus #3): `readMaterialFile` returns `std::nullopt` for:
    - a file containing `not json`;
    - `{"fumarMaterial": 2}`;
    - `{"fumarMaterial": 1, "uvScale": "big"}`;
    - a missing file.

    None of them throws.
  - **`fmatMissingTexture`:** a `.fmat` naming `gone.jpg` reads successfully with `baseColor` set to the absolute path of `gone.jpg`. The GPU side warns and uses neutral; the smoke covers that.
- [ ] **Step 2: Run it and confirm it fails.** It does not compile.
- [ ] **Step 3: Implement it.**
  - `texture_set.cpp`: the suffix table is spec §2.2, kept in a `constexpr` array in priority order. Pure file-name logic, plus `std::filesystem::directory_iterator` in the folder overload.
  - `material_file.cpp`: nlohmann with `parse(..., nullptr, false)` and explicit type checks, as `gltf_loader` reads extras.
  - `Renderer::createMaterial(desc, origin)`:
    - Each path loads with the slot's colour space. A file that is missing or fails to decode logs `FUMAR_WARN("material '{}': cannot read {} '{}', using the neutral map", ...)` and leaves the slot empty.
    - Separate roughness and metallic files load their pixels (stb) and go through `packMetallicRoughness`, then upload as one linear texture.
    - A DirectX normal flips G on the CPU before upload.
- [ ] **Step 4: Put `.fmat` in scenes.**
  - `scene_io` writes `{"fmat": "<path relative to the scene>"}` for `Fmat` origins, and reads it with `loadMaterialFile`.
  - On failure (Review Focus #5) it logs `FUMAR_WARN("scene '{}': material file '{}' is missing; using the default", ...)`, inserts the fallback material at that index so later indices still line up, and the load still returns true.
- [ ] **Step 5: Handle folder drops** in `main.cpp`.
  - The dropped-files loop: for a directory whose import made a material, select nothing and log `FUMAR_INFO("material '{}' made from '{}'", ...)`.
  - When an object is selected, also assign the new material to it as an undo step (`state.history.record` first).
- [ ] **Step 6: Run the tests.** `material_tests` exits 0; debug smoke `finish` and `save` show no VUIDs.
- [ ] **Step 7: Add the smoke variant `materials`** in `runModelerSmoke`, debug, validation on. Frame by frame:
  1. Write a temp folder `<temp>/fumar_smoke_set/` with three generated PNGs (stb_image_write is in stb; add the implementation define in one `.cpp` if needed): `smoke_Color.png` 64×64 red, `smoke_NormalGL.png` flat, `smoke_Roughness.png` grey. Call `renderer.importAsset(folder)`. Expected: a material named "fumar_smoke_set" with three slots.
  2. Assign it to Block A.
  3. Set roughness to 0.1 through `markMaterialDirty`. Two frames later, log the record value read back from `describeMaterial`.
  4. Delete `smoke_Roughness.png`, edit the `.fmat` text to name `gone.png`, and `loadMaterialFile` a copy at a new path. Expected: a valid handle and one "cannot read" warning (Review Focus #2).
  5. Save the scene to `<temp>/smoke_materials.fumar`, delete the copy's `.fmat` (Review Focus #5), and load the scene. Expected: `loaded`, one "is missing" warning, and Block A still has the first material.

  Each step logs `modeler smoke: materials <step>: ok|FAILED`.
- [ ] **Step 8: Commit.** "Material files, texture-set folders, and materials that scenes point at"

### Task 6: Material edits in the undo history

**Files:**
- Modify: `editor/src/history.hpp`, `editor/src/history.cpp`, and the call sites of `History::undo/redo` in `editor/src/main.cpp` (they need the `Renderer`, or a `ResourceRegistry&` and a dirty callback)
- Test: smoke `materials` (Task 5's variant gains steps)

**Interfaces:**
- Consumes: `Renderer::markMaterialDirty`, `ResourceRegistry::material(MaterialHandle)` (Task 3).
- Produces:
  - `struct MaterialParams` holds every `Material` field except the `TextureHandle`s' GPU objects; the handles themselves are copied. It is in `history.hpp`.
  - `SceneSnapshot::materials` is a `std::vector<MaterialParams>`, indexed by `MaterialHandle::index`.
  - `History::record(const Scene&, const ResourceRegistry&, NodeId selected)`.
  - `History::undo/redo(Scene&, ResourceRegistry&, NodeId current, NodeId& restored, std::vector<MaterialHandle>& changed)`. The caller marks `changed` dirty. Materials past the snapshot's size (created after it) are left alone.
  - Every existing `record(scene, selected)` call site gains `renderer.resources()`.

- [ ] **Step 1: Extend the `materials` smoke** (the failing test):
  1. Record, set Block A's material roughness from 0.1 to 0.9, mark it dirty.
  2. Raise `state.undoRequested`.
  3. Two frames later, expect the roughness back at 0.1 ("materials undo: ok").
  4. Redo, and expect 0.9.
- [ ] **Step 2: Run it and confirm it fails.** Debug smoke `materials` prints "materials undo: FAILED".
- [ ] **Step 3: Implement** the snapshot, restore and changed-list, and update every call site. `grep -n "history.record" editor/src` lists them; there are 24.
- [ ] **Step 4: Run the tests.** Smoke `materials` all ok, `finish` ("scene undo restored the mesh") ok, no VUIDs.
- [ ] **Step 5: Commit.** "Undo and redo material edits with the scene"

### Task 7: The Materials panel

**Files:**
- Create: `engine/platform/include/fumar/platform/file_dialog.hpp`, `engine/platform/src/file_dialog.cpp`, `editor/src/materials_panel.hpp`, `editor/src/materials_panel.cpp`
- Modify: `engine/platform/CMakeLists.txt`, `editor/CMakeLists.txt` (if sources are listed), `editor/src/modeler.cpp` (remove `drawMaterialsPanel`), `editor/src/modeler.hpp`, `editor/src/main.cpp`, `editor/src/panels.hpp` (`EditorState` fields), `engine/render/include/fumar/render/renderer.hpp` (texture views for thumbnails, if not exposed)

**Interfaces:**
- Consumes: `loadMaterialFile`, `saveMaterialFile`, `createMaterial(desc, origin)`, `describeMaterial`, `writeMaterialFile`, `markMaterialDirty`, `History` (Tasks 3, 5, 6). `ImGuiLayer::registerTexture(vk::ImageView, vk::Sampler)` / `unregisterTexture`.
- Produces:
  - `file_dialog.hpp`: `class FileDialog { public: void openFile(Window&, std::span<const FileFilter>, const std::filesystem::path& start); std::optional<std::filesystem::path> takeResult(); bool pending() const; };` with `struct FileFilter { const char* name; const char* pattern; };` (the SDL format, e.g. `"jpg;jpeg;png;tga"`).
    - Wraps `SDL_ShowOpenFileDialog`. The callback may run on another thread, so it stores the result under a mutex, and `takeResult()` is polled each frame.
    - Only one dialog is open at a time.
  - `void drawMaterialsPanel(EditorState&, Renderer&, ImGuiLayer&, FileDialog&);` in `materials_panel.hpp`.
  - `EditorState` gains:
    - `MaterialHandle editedMaterial`;
    - `std::string materialFilter`;
    - `std::unordered_map<u32, ImTextureID> materialThumbnails`, keyed by texture index and released when the registry is replaced;
    - `int pendingMapSlot = -1`;
    - `std::set<u32> modifiedMaterials`, the material indices edited since the last save.

- [ ] **Step 1: Extend the `materials` smoke** (the failing test), driving the panel's model functions rather than clicks:
  - `duplicateAsFmat(state, renderer, helmetMaterial)` returns a handle whose origin is `Fmat`, whose file exists, and whose five textures are written as PNGs beside it ("materials duplicate: ok").
  - `newMaterial(state, renderer)` creates `<exe>/materials/New Material.fmat`, and a second call creates `New Material 2.fmat`.

  Expose both as free functions in `materials_panel.hpp`, so the panel and the smoke share them.
- [ ] **Step 2: Run it and confirm it fails.** It does not compile.
- [ ] **Step 3: Implement the panel** (spec §3.2).
  - **Library on the left:**
    - Sources: scene materials; `assets/materials/**/*.fmat` (the stock library, scanned once and on a Rescan button); `<exe>/materials/*.fmat`.
    - A thumbnail of `baseColor` through `registerTexture`, or a colour button. Filter box, New button.
    - Click assigns, with `history.record` before.
    - With no `assets/materials` directory, show: "Stock materials are on the `materials` branch - run tools/get-materials to fetch them."
  - **Editor on the right:**
    - Five slots, each with a thumbnail, the file name, `…` (opens `FileDialog` with an image filter and sets `pendingMapSlot`) and `×`. The normal slot has a GL/DX toggle.
    - Factors with ImGui sliders and colour edits.
    - Undo records on `IsItemActivated` with the pre-edit params (`MaterialParams` captured before the widget, as the smooth-angle fix does).
    - `markMaterialDirty` on change; the material is added to `state.modifiedMaterials` (a `std::set<u32>`).
  - **Map slots after a pick.** When `takeResult()` yields a path, rebuild the material's textures through `createMaterial(describeMaterial(m) with the slot replaced, origin)` and copy the result into the same handle (`resources.material(h) = …`), so nodes keep pointing at it. Then mark it dirty.
  - **Read-only materials:** glTF and Builtin origins show the fields disabled, plus a **Duplicate as .fmat** button.
  - **Ctrl+S:** in `main.cpp`, before `saveScene`, `saveMaterialFile` every handle in `modifiedMaterials`, then clear the set.
- [ ] **Step 4: Run the tests.** Smoke `materials` all ok; `finish`, `save`, `orbit` ok; no VUIDs; `geometry_tests` and `material_tests` exit 0.
- [ ] **Step 5: Check the panel by eye.**
  - Release build. Start the editor, switch to Modeler, select the helmet.
  - Screenshot of the Materials panel showing the library with thumbnails and the read-only helmet material.
  - Then a smoke-driven state (`materials` variant, after its duplicate step) screenshot showing the editable copy's fields.
  - The user tries the panel interactively; do not simulate input.
- [ ] **Step 6: Commit.** "A material library and editor in the Modeler"

### Task 8: Stock materials on the `materials` branch

**Files:**
- Branch `materials` (orphan): `assets/materials/<Name>/*`, `assets/materials/SOURCES.md`
- Create on `main`: `tools/get-materials.ps1`, `tools/get-materials.sh`, `docs/materials.md`, `docs/materials.pl.md`
- Modify on `main`: `editor/src/main.cpp` (`createStarterScene`)

**Interfaces:**
- Consumes: `loadMaterialFile` (Task 5).

- [ ] **Step 1: Choose and download ten ambientCG sets** at `2K-JPG`, in a scratch folder outside the repo, from `https://ambientcg.com/get?file=<AssetId>_2K-JPG.zip`.
  - The ten: bricks, wood planks, grass, concrete, brushed metal, plaster, tiles, rock, asphalt, fabric.
  - Each must contain at least Color, NormalGL and Roughness. Check after unzipping, and pick another id if not.
  - Record each id and URL.
- [ ] **Step 2: Build the branch.**
  - Work in a separate worktree: `git worktree add ../fumar-materials --orphan materials`. If that git is too old for `--orphan`, use `git switch --orphan` in the worktree.
  - Copy each set into `assets/materials/<Name>/`, dropping displacement maps and preview images.
  - Write each `<Name>.fmat` by hand, following spec §3.1, from the file names in its folder. There are ten. Then load each one once through the editor (drop the folder) to confirm it reads.
  - Write `SOURCES.md` with the asset ids, URLs and "CC0 1.0 - ambientCG, https://ambientcg.com".
  - Commit with `git add -f`, because `assets/*` is ignored on `main` and the ignore file is shared. Push `materials`. It is a public repo; this branch is the agreed plan.
- [ ] **Step 3: Write the scripts.**
  - **`tools/get-materials.ps1`:** `git -C <repo> fetch origin materials`, then `git -C <repo> archive origin/materials assets/materials`, extracted with `tar -x -C <repo>` (`tar` ships with Windows 10+).
  - **`tools/get-materials.sh`:** the same in sh.
  - Both print how many materials landed, and fail with exit 1 and a message if the fetch fails.
- [ ] **Step 4: Write the docs.** `docs/materials.md` + `docs/materials.pl.md`:
  - the stock library and how to fetch it;
  - `git clone --single-branch` for `main` alone, with the ~100 MB note;
  - the folder-naming table (spec §2.2), with drop-a-folder import;
  - the `.fmat` format (spec §3.1);
  - the RT alpha-mask limitation (spec §1.5);
  - that release zips do not contain the stock library.
- [ ] **Step 5: Use the stock materials in the starter scene.** `createStarterScene` uses `assets/materials/Bricks/Bricks.fmat`, `Wood…/…fmat` and `Grass…/…fmat` through `loadMaterialFile` when they exist, and today's `createMaterial(name, colour, jpg)` otherwise. The folder names are the ones chosen in step 1.
- [ ] **Step 6: Verify.**
  - Run `tools/get-materials.ps1` on this machine; it reports 10 materials.
  - Release build; the starter scene uses them.
  - Screenshots before and after (same camera) of the courtyard, plus a 4× crop of a normal-mapped brick wall.
  - Then temporarily move `assets/materials` aside, start the editor, and confirm the scene opens with the old textures and the Materials panel shows the "run tools/get-materials" line. Move it back.
  - Debug smoke `finish`, `save`, `helmet`, `materials`: no VUIDs. `helmet` is ≤ 2.6 ms in release.
- [ ] **Step 7: Commit on `main`.** "Stock materials on their own branch, and the starter scene wearing them"
- [ ] **Step 8: Update the memory file** `fumar-stan-renderera.md`: materials state, branch `materials`, `get-materials`, the smoke `materials` variant.
