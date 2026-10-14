# PBR materials

Sub-project 1 of the "make it look good" track. The others, each with its own spec later:

| # | Sub-project | Purpose |
|---|---|---|
| 2 | Velocity buffer | Stop moving objects smearing in the temporal filter |
| 3 | Cubemaps | Sky IBL with prefiltered mips, then reflection probes |
| 4 | Ray tracing | Rough RT reflections, denoising |

## Goal

Lighting is already ray traced, but every surface is one colour texture with a constant metallic and roughness. This sub-project makes materials carry what real assets carry, and makes them easy to get and to edit:

- **Maps:** base colour, metallic-roughness, normal, occlusion, emissive, plus an alpha cutout.
- **Three sources:** glTF from Blender, texture sets dropped from disk, and a stock library.
- **An editor:** materials editable live in the Modeler's Materials panel, and saved as `.fmat` files reusable across scenes.

Success looks like this:

- `DamagedHelmet.glb` shows its normal, metal-rough, occlusion and emissive maps.
- A folder downloaded from ambientCG or Poly Haven becomes a material by dropping it on the editor.
- The stock library offers about ten 2K materials.
- A material's sliders change the viewport live, with undo.
- Ray-traced reflections and GI see the same materials the camera does.

## Decisions taken

| Decision | Choice |
|---|---|
| Architecture | **One material buffer and one texture array shared by raster and ray tracing** (approach A), rather than per-material descriptor sets plus a parallel RT copy. |
| Stock materials | 2K sets from ambientCG (CC0), on a separate branch `materials`. `main` carries a fetch script and a note. |
| Material sources | All three: glTF, texture-set folders, stock, plus an editor and `.fmat` files. |
| Tangents | Read from glTF, or computed with MikkTSpace on import. Meshes edited in the Modeler use cheap UV-gradient tangents instead (see §1.3). |

## Out of scope

- Displacement and parallax mapping.
- Real transparency. Alpha *blend* is drawn as opaque.
- Rendered preview spheres in the material library.
- Rough ray-traced reflections (sub-project 4).
- An alpha test inside ray queries: masked surfaces cast solid shadows in RT for now (§1.5).

---

## 1. Data and GPU

### 1.1 `Material` (CPU, `engine/render/include/fumar/render/resources.hpp`)

```cpp
enum class AlphaMode : u8 { Opaque, Mask };

struct Material {
    std::string name;

    TextureHandle baseColor;          // sRGB
    TextureHandle metallicRoughness;  // linear; G = roughness, B = metallic (glTF layout)
    TextureHandle normal;             // linear, OpenGL convention (+Y up)
    TextureHandle occlusion;          // linear; R
    TextureHandle emissive;           // sRGB

    Vec4 baseColorFactor{1, 1, 1, 1};
    f32 metallic = 0.0f;              // multiplies the map's B
    f32 roughness = 0.75f;            // multiplies the map's G
    f32 normalScale = 1.0f;
    f32 occlusionStrength = 1.0f;
    Vec3 emissiveColor{0, 0, 0};
    f32 emissiveIntensity = 1.0f;     // KHR_materials_emissive_strength
    AlphaMode alphaMode = AlphaMode::Opaque;
    f32 alphaCutoff = 0.5f;
    Vec2 uvScale{1, 1};

    MaterialOrigin origin;            // where it came from, for saving (§3.1)
};
```

**Slots without a map** point at fixed neutral textures created by the renderer, so shaders never branch on "is there a map":

| Slot | Neutral texture |
|---|---|
| base colour, metal-rough, occlusion, emissive | white |
| normal | `(0.5, 0.5, 1)` |

Emissive reads white, not black: glTF's emission is the colour times the map, so with no map the colour alone has to come through. (The first version of this spec said black, which switched off every emissive colour without a texture; fixed during Task 4.)

The per-material `vk::DescriptorSet` and the material set layout's use for materials go away. The bloom and post passes keep a layout of their own, which they currently borrow from materials.

### 1.2 GPU layout

**`MaterialRecord`** (scalar layout, 64 bytes), declared in `shaders/material.glsl` and mirrored in C++:

```glsl
struct MaterialRecord {
    vec4  baseColorFactor;
    vec3  emissive;          // colour * intensity, premultiplied on upload
    float metallic;
    float roughness;
    float normalScale;
    float occlusionStrength;
    float alphaCutoff;       // < 0 means opaque
    vec2  uvScale;
    uint  baseColorTexture;
    uint  metallicRoughnessTexture;
    uint  normalTexture;
    uint  occlusionTexture;
    uint  emissiveTexture;
    uint  padding;
};
```

**Material buffer.** Set 0 gets a new binding, `materials`, a read-only storage buffer.

- There is one buffer per frame in flight.
- A material edit marks the material dirty for every frame. At the start of a frame, after that frame's fence has been waited on, its dirty records are rewritten.
- So there is no `waitIdle`, and an edit is visible on the next frame.
- The buffer grows (reallocate, then rewrite the descriptor) when the material count passes its capacity, which starts at 256.

**Texture array.** `sceneTextures` (set 0, binding 3) grows from 128 to **1024** slots. It now exists on **every** GPU, not only with ray tracing.

- Slot `i` is texture handle `i`.
- New textures are written the same way as material records: per frame in flight, after that frame's fence.
- This replaces `refreshTextureArray`'s `waitIdle`.
- Slots beyond the scene are pre-filled with the white neutral texture once, at startup.
- No descriptor-indexing features are needed beyond what is already enabled.
- Raster indexes the array with a value uniform per draw. Ray hits use `nonuniformEXT`, which needs `shaderSampledImageArrayNonUniformIndexing`; that is already enabled with ray tracing.

**Push constants** (`object.glsl` / `ObjectPushConstants`): `mat4 model`, `vec4 overlayColor`, `uint materialIndex`, `float highlight`. That is 88 bytes, down from 92. `overlayColor` is today's `baseColor`, renamed: `outline.frag` draws with it when `highlight < 0`, and nothing else reads it. The material's colour, metallic, roughness and UV scale move into the material record.

**`InstanceRecord`** (RT): vertex address, index address, `uint materialIndex`. Its colour, metallic, roughness, texture and uvScale fields go.

### 1.3 Tangents

`Vertex` gains `Vec4 tangent`, with `w` = bitangent sign. It grows from 32 to 48 bytes. This touches:

- `Vertex::attributes()` (location 3);
- `mesh.vert` (outputs a world-space tangent);
- the `buffer_reference` vertex struct in `raytrace.glsl`;
- `gltf_writer` (writes `TANGENT`);
- `gltf_loader` (reads `TANGENT`).

Two sources:

- **Imported meshes.** The glTF `TANGENT` attribute is used if present. Otherwise tangents come from **MikkTSpace**, the reference used by Blender, so baked normal maps match.
  - It is vendored via FetchContent (`mmikk/MikkTSpace`, pinned commit, zlib licence) as a small static library used by `fumar_render`.
  - It is wrapped as `computeTangents(std::span<Vertex>, std::span<const u32>)` in `engine/render/src/tangents.cpp`.
- **Meshes edited in the Modeler.** `EditableMesh::triangulate()` emits tangents itself: a per-face tangent from the UV gradient, averaged across the same smoothing groups as the normals, then Gram-Schmidt orthogonalised.
  - This is O(corners), and keeps the per-frame drag cost near today's 2.2 ms (helmet).
  - `TriangleMesh` gains `std::vector<Vec4> tangents`.
  - MikkTSpace is not used here, because it would multiply the cost of every frame of a drag.

**Degenerate UVs** (zero UV area) get a tangent from any vector perpendicular to the normal, so no NaNs reach the shaders.

### 1.4 Shaders

**`shaders/material.glsl`** (new) is included by `mesh_shading.glsl` and `raytrace.glsl`:

```glsl
struct MaterialSample {
    vec3 albedo; float alpha; vec3 normal; float roughness;
    float metallic; float occlusion; vec3 emissive;
};
MaterialSample sampleMaterial(uint index, vec2 uv, mat3 tbn);   // raster
MaterialSample sampleMaterialHit(uint index, vec2 uv, vec3 n);  // ray hit: no normal map, textureLod level 0 as today
```

**Raster** (`mesh.frag` / `mesh_rq.frag`):

- The normal map is applied through the TBN, scaled by `normalScale`.
- Occlusion multiplies the ambient and indirect terms only, not direct light.
- Emissive is added after lighting.
- `Mask` discards below the cutoff.

**Ray hits** (reflections and GI bounce):

- They read albedo, roughness, metallic and emissive.
- An emissive surface hit by a GI ray contributes its emission, so a glowing panel lights what is near it.
- The normal map is skipped at hits. In a reflection or a bounce it is not visible, and it would cost a TBN per hit.

**Outline / selection** (`outline.frag`): it declares `baseColorTexture` at set 1 but never samples it. The declaration goes, along with set 1 from its pipeline layout. It keeps drawing with `object.overlayColor`.

### 1.5 Alpha cutout

- **Raster:** `discard` in `mesh.frag`.
- **Shadow pass:** none exists. Shadows are ray traced.
- **RT:** instances stay `eOpaque`, so masked surfaces block rays as solid quads. This is a known limitation, listed in the docs. Fixing it means non-opaque instances and a candidate alpha test in each ray-query loop; it belongs with sub-project 4.

---

## 2. Where materials come from

### 2.1 glTF (`engine/render/src/gltf_loader.cpp`)

The loader reads:

- `pbrMetallicRoughness`: base colour, metal-rough texture and factors;
- `normalTexture.scale`;
- `occlusionTexture.strength`;
- `emissiveTexture`, `emissiveFactor`, and `KHR_materials_emissive_strength`;
- `alphaMode` / `alphaCutoff` (`BLEND` is loaded as opaque, with a debug log line).

**Texture colour space** is decided by use. A glTF image used both as colour and as data is loaded twice, once in each format. In practice that does not happen.

- base colour and emissive: sRGB;
- metal-rough, normal, occlusion: linear.

Textures are only created when a material references them, so an unused image is not uploaded.

### 2.2 Texture-set folders

Dropping a **folder** on the editor makes a material from it. `Renderer::isImportable` accepts directories; SDL reports a dropped directory as a path.

**Maps are recognised by file-name suffix**, case-insensitive, before the extension. Accepted extensions: `.jpg`, `.jpeg`, `.png`, `.tga`. When several files match one slot, the first in this table wins.

| Slot | Suffixes |
|---|---|
| base colour | `_Color`, `_BaseColor`, `_Albedo`, `_diff`, `_Diffuse` |
| normal (OpenGL) | `_NormalGL`, `_nor_gl`, `_Normal` |
| normal (DirectX, Y flipped on load) | `_NormalDX`, `_nor_dx` |
| roughness | `_Roughness`, `_rough` |
| metallic | `_Metalness`, `_Metallic`, `_metal` |
| occlusion | `_AmbientOcclusion`, `_AO`, `_ao` |
| emissive | `_Emission`, `_Emissive` |

**Separate roughness and metalness images** are packed on the CPU into one RGBA8 texture in glTF layout. Roughness goes to G and metalness to B; R and A are 255. When the sizes differ, the smaller image is resampled (bilinear) to the larger. No metalness map means B = 255, which the factor then scales (default 0).

**The result** is written to `<folder>/<folder name>.fmat` and loaded from there, so a dropped folder and a stock material follow the same path.

The **suffix matching** lives in `engine/render/src/texture_set.cpp` as `TextureSetFiles findTextureSet(const std::filesystem::path& folder)`. It is pure file-name logic and unit-tested.

### 2.3 Stock library (branch `materials`)

**The branch** is an orphan branch `materials`. It holds only `assets/materials/<Name>/` folders and `assets/materials/SOURCES.md`:

- Each folder holds the 2K JPG maps from ambientCG and `<Name>.fmat`.
- `SOURCES.md` lists each set's ambientCG id and URL and the CC0 licence.
- Ten sets: Bricks, Wood planks, Grass, Concrete, Metal (brushed), Plaster, Tiles, Rock, Asphalt, Fabric.
- The exact asset ids are chosen while building the branch. Each one needs colour, normal (GL) and roughness at least.

**On `main`:**

- **`tools/get-materials.ps1` / `tools/get-materials.sh`:** `git fetch origin materials`, then `git archive origin/materials assets/materials | tar -x` into the repository root. Nothing is staged, and `assets/*` is already git-ignored.
- **`docs/materials.md` + `docs/materials.pl.md`:**
  - where the stock library lives and how to get it;
  - `git clone --single-branch` for anyone who wants `main` without it;
  - the folder-naming rules from §2.2;
  - the `.fmat` format.
- **The starter scene** uses `assets/materials/Bricks/Bricks.fmat` (and Wood, Grass) when present, and today's `assets/textures/*.jpg` otherwise. A fresh clone without the branch still opens.
- **Release builds** (CI zip) do not include the stock library. The docs say so.

`git clone` without `--single-branch` downloads the branch's objects too (about 100 MB). That is accepted; the docs say so.

---

## 3. Editor and files

### 3.1 `.fmat` format

JSON, UTF-8, with paths relative to the `.fmat` file:

```json
{
  "fumarMaterial": 1,
  "name": "Bricks",
  "baseColor": { "texture": "Bricks_Color.jpg", "factor": [1, 1, 1, 1] },
  "metallicRoughness": { "roughness": "Bricks_Roughness.jpg", "metallic": null,
                         "roughnessFactor": 1.0, "metallicFactor": 0.0 },
  "normal": { "texture": "Bricks_NormalGL.jpg", "scale": 1.0, "convention": "gl" },
  "occlusion": { "texture": "Bricks_AmbientOcclusion.jpg", "strength": 1.0 },
  "emissive": { "texture": null, "color": [0, 0, 0], "intensity": 1.0 },
  "alpha": { "mode": "opaque", "cutoff": 0.5 },
  "uvScale": [1, 1]
}
```

`metallicRoughness` takes **either** separate `roughness` / `metallic` paths, packed on load, **or** a single `"packed"` path in glTF layout.

**`MaterialOrigin`** records how a material is saved:

| Origin | Saved as |
|---|---|
| `Builtin` | today's `texture` + colour |
| `Gltf` | `{file, index}` as today |
| `Fmat` | `{path}` |

**Scenes** reference `.fmat` materials by path, relative to the scene file. Existing scene files keep loading: the old `texture` / `file` + `index` entries are read as before.

**The reader and writer** live in `engine/render/src/material_file.cpp`:

```cpp
std::optional<MaterialDesc> readMaterialFile(const std::filesystem::path&);
bool writeMaterialFile(const std::filesystem::path&, const MaterialDesc&);
```

- `MaterialDesc` is the CPU-side description: factors and absolute paths, no handles.
- `Renderer::createMaterial(const MaterialDesc&)` turns one into GPU resources.
- Writing goes through a temporary file renamed over the target, as scene saving does.

### 3.2 Materials panel (Modeler)

The panel replaces today's swatch grid. A splitter divides it into two halves.

**Library (left):**

- **Sources:** materials in the scene (including glTF ones), stock `.fmat` files under `assets/materials/`, and user `.fmat` files under `<exe>/materials/`.
- Each entry is a thumbnail of its base-colour texture (or its colour) with its name. Thumbnails reuse the texture's view through `ImGuiLayer::registerTexture`.
- A filter box narrows the list by name.
- **New** creates `<exe>/materials/New Material.fmat` (unique name) and selects it.
- **Click** assigns the material to the selected object, as an undo step. A `.fmat` that is not loaded yet is loaded first.
- **When the stock library is missing**, one line says so and names `tools/get-materials`.

**Editor (right)** works on the material of the selected object, or the one picked in the library:

- Name.
- Five map slots, each with:
  - a thumbnail;
  - the file name;
  - **…**, which opens a file picker. The editor already has a native picker for scenes; that one is reused.
  - **×**, which clears the slot.

  The normal slot also has a GL/DX toggle.
- Base colour factor (colour picker), metallic, roughness, normal scale, occlusion strength, emissive colour and intensity, UV scale, alpha mode and cutoff.
- **Every change applies live** (§1.2) and marks the material modified.
- **Ctrl+S** saves the scene and every modified `.fmat`.
- **Read-only materials:** glTF and built-in materials show **Duplicate as .fmat** instead of editable fields. It makes an editable copy beside the user materials, with the glTF's embedded textures written out as PNG files, and assigns it.

### 3.3 Undo

`SceneSnapshot` gains the material parameters: everything in `Material` except the texture handles' GPU objects, which persist in the registry. Undo restores them and marks the materials dirty for upload.

- Slider drags record once, on activation, with the value from before the drag (as the smooth-angle fix did).
- Undoing **New** leaves the material in the registry, unused, as meshes are today.

---

## 4. Testing

**`tools/geometry_tests`:**

- Tangents from `triangulate()` on the cube, plane and UV sphere are unit length, orthogonal to the normal, with `w` = ±1.
- A face with degenerate UVs yields a finite tangent.

**New `tools/material_tests`** (headless, in CI next to `geometry_tests`):

- `findTextureSet` on fixture file lists: ambientCG names, Poly Haven names, mixed case, DX normals, a missing roughness map.
- A `.fmat` round trip: write, read, compare.
- Roughness and metalness packing on two small in-memory images of different sizes.
- MikkTSpace on a quad with known UVs gives the expected tangent.

**Smoke** (`FUMAR_MODELER_SMOKE`), debug build, validation layers:

- **`materials`:**
  1. Load the helmet and check its material has all five maps.
  2. Create a `.fmat` from a temp folder of generated images.
  3. Assign it to Block A.
  4. Change roughness and check the record uploaded on the next frame.
  5. Undo and check the value came back.
  6. Save the scene, reload it, and check the material and its maps came back.
- **Existing variants** (`finish`, `save`, `orbit`, `helmet`) still pass. `helmet` still reports the drag cost; the target is at most 2.6 ms, against 2.2 ms today.

**Screenshots:** before and after, same camera, of the helmet and the courtyard with stock materials. Plus a 4x crop of a normal-mapped brick wall.

**Zero VUIDs** throughout.

---

## 5. Order of work

Each step builds, passes the tests above that apply to it, and is committed and pushed:

1. **`Vertex.tangent`**, MikkTSpace and `triangulate()` tangents, plus tests. Nothing uses the tangents yet.
2. **Material buffer, 1024-slot texture array, per-frame descriptor updates**, and the push constant and instance record changes. Visually identical to today.
3. **`material.glsl` and the five maps** in raster and RT, plus emissive in GI. glTF loading of all maps; the helmet now looks right.
4. **`.fmat` read/write**, texture-set folders, and the drop-a-folder import, plus `material_tests`.
5. **Materials panel**, library and editor, plus material undo.
6. **The `materials` branch** with the stock sets, `get-materials` scripts, docs (EN + PL), and the starter scene using the stock materials when present.
