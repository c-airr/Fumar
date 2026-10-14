# Materials

*[Polska wersja](materials.pl.md)*

A fumar material is physically based, in the metallic-roughness model glTF uses. It has five optional maps:

| Map | Colour space | What it holds |
|---|---|---|
| colour | sRGB | the surface's colour (and alpha, for cutouts) |
| metal-rough | linear | roughness in **G**, metalness in **B** (glTF's layout) |
| normal | linear | tangent-space normals, OpenGL convention (+Y up) |
| occlusion | linear | crevice darkening in **R** |
| emission | sRGB | light the surface gives off |

Each map is multiplied by a factor: colour, metallic, roughness, normal strength, occlusion strength, emission colour × intensity. A missing map counts as white, or as a flat normal for the normal map, so the factor alone decides.

Two more settings:
- **Alpha:** opaque, or **cutout**, which discards pixels below a threshold.
- **Tiling:** how many times the maps repeat across the mesh.

Materials come from three places.

## 1. The stock library

Ten 2K texture sets from [ambientCG](https://ambientcg.com), all CC0 (public domain): bricks, wood floor, grass, concrete, metal, plaster, tiles, rock, asphalt, fabric. Their sources are listed in `assets/materials/SOURCES.md`.

They are about 110 MB, so they live on their own branch, **`materials`**, not on `main`. To get them:

```sh
tools/get-materials.sh        # Linux, Git Bash
tools\get-materials.ps1       # Windows PowerShell
```

The script fetches the branch and unpacks `assets/materials` into your checkout. It does not switch branches and does not touch the index, because `assets/` is git-ignored on `main`. Run it again to update.

**Without the library:**
- The starter scene falls back to the plain colour textures in `assets/textures`.
- The Materials panel says where the library is.

**Release builds** (the CI zips) do not include the library either.

**Cloning:** a plain `git clone` downloads every branch, the library included. To get `main` alone:

```sh
git clone --single-branch https://github.com/c-airr/Fumar.git
```

## 2. Texture-set folders

Drop a folder of maps onto the editor and it becomes a material. Maps are recognised by the suffix of their file name. Case does not matter. Accepted formats are `.jpg`, `.jpeg`, `.png` and `.tga`.

| Map | Suffixes, in order of preference |
|---|---|
| colour | `_Color`, `_BaseColor`, `_Albedo`, `_diff`, `_Diffuse` |
| normal (OpenGL) | `_NormalGL`, `_nor_gl`, `_Normal` |
| normal (DirectX, flipped on load) | `_NormalDX`, `_nor_dx` |
| roughness | `_Roughness`, `_rough` |
| metalness | `_Metalness`, `_Metallic`, `_metal` |
| occlusion | `_AmbientOcclusion`, `_AO` |
| emission | `_Emission`, `_Emissive` |

A suffix counts only where it ends the name, or is followed by `_`, `-` or `.`. So `_Normal` does not claim `_NormalGL`, and Poly Haven's `brick_wall_diff_2k.jpg` still matches `_diff`. Anything else in the folder is ignored: displacement maps, previews, `.blend` files. A folder with neither a colour nor a normal map makes no material.

What the import does:
- Separate roughness and metalness images are packed into one texture in glTF's layout.
- The result is written to **`<folder>/<folder name>.fmat`** and loaded from there.
- Dropping the same folder again loads that file. It never regenerates it, so edits are not lost.
- Dropped while an object is selected, the material goes onto that object.

## 3. glTF files

Models from Blender (or anywhere) bring their materials with them:
- all five maps;
- `KHR_materials_emissive_strength`;
- the alpha mode. `BLEND` is drawn opaque: real transparency is not implemented yet.

These materials belong to their file and are **read-only** in the editor. **Duplicate as .fmat** in the Materials panel makes an editable copy, writing the file's embedded maps out as PNG files beside it.

## Editing: the Materials panel

The Materials panel is in the **Modeler**, where Content is in the Level. It has two halves.

**Left, the library:**
- the scene's materials, then the stock and user `.fmat` files not loaded yet;
- a filter box;
- **New**, which makes `materials/New Material.fmat` beside the editor;
- clicking a material puts it on the selected object.

**Right, the editor** shows the selected object's material, or the one last clicked:
- **Five map slots:** `...` opens the system file dialog, `×` clears the slot. The normal slot has a DirectX toggle.
- **Every factor** as a slider or colour picker. Changes show in the viewport immediately.
- **Undo:** every change is undoable with Ctrl+Z, one step per drag.
- **Saving:** Ctrl+S writes each edited `.fmat`, then the scene. "Saved" appears only if all of them were written.

In a development build, the stock library is read from `assets/materials` in the source tree. Editing a stock material there changes your local copy, and no build overwrites it.

## The `.fmat` format

JSON. Paths are relative to the `.fmat` file, so a folder of materials can be moved as a whole.

```json
{
  "fumarMaterial": 1,
  "name": "Bricks",
  "baseColor": { "texture": "Bricks_Color.jpg", "factor": [1, 1, 1, 1] },
  "metallicRoughness": {
    "packed": null,
    "roughness": "Bricks_Roughness.jpg",
    "metallic": null,
    "roughnessFactor": 1.0,
    "metallicFactor": 0.0
  },
  "normal": { "texture": "Bricks_NormalGL.jpg", "scale": 1.0, "convention": "gl" },
  "occlusion": { "texture": "Bricks_AmbientOcclusion.jpg", "strength": 1.0 },
  "emissive": { "texture": null, "color": [0, 0, 0], "intensity": 1.0 },
  "alpha": { "mode": "opaque", "cutoff": 0.5 },
  "uvScale": [1, 1]
}
```

- **Metal-rough:** give either one `packed` image in glTF's layout, or separate `roughness` and/or `metallic` images.
- **Missing fields** take their defaults.
- **Refused files:** a file of a different `fumarMaterial` version, or with a field of the wrong type, is refused with a warning.
- **Missing texture:** a texture that cannot be read leaves its slot neutral, with a warning. The path is kept, so putting the file back fixes it.

**In scenes**, a `.fmat` material is saved as a reference, `{"name": ..., "fmat": <path>}`, relative to the executable's directory like every other path in a scene file. A scene whose `.fmat` has gone missing still loads, and the objects that used it show the default material.

## Limits

- **Cutouts in ray tracing:** a cutout material casts a solid shadow and blocks reflections and bounced light as if it had no holes. The rays treat all geometry as opaque.
- **No transparency yet:** alpha blending is drawn opaque.
- **No displacement or parallax mapping yet.**
- **Size limits:** maps larger than the GPU's texture limit are left out, with a warning. A scene reaches at most 1024 textures; any beyond that read as white, with a warning.
