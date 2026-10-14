// Tests for the parts of the material system that run without a GPU.
//
// Same shape as geometry_tests: each check prints PASS or FAIL with what it
// expected, and the exit code says whether any failed.

#include "fumar/geometry/primitives.hpp"
#include "fumar/render/material_file.hpp"
#include "fumar/render/material_gpu.hpp"
#include "fumar/render/mesh.hpp"
#include "fumar/render/tangents.hpp"
#include "fumar/render/texture_set.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace fumar;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    std::printf(condition ? "  PASS  %s\n" : "  FAIL  %s\n", what.c_str());
    if (!condition) {
        ++failures;
    }
}

void section(const char* name) { std::printf("\n%s\n", name); }

bool near(f32 a, f32 b, f32 tolerance) { return std::abs(a - b) <= tolerance; }

// --- tangents ------------------------------------------------------------------

/// A unit quad on the ground, facing up, with U along +X and V along +Z.
///
/// The tangent follows U, so it is +X. The handedness w is whatever turns
/// cross(normal, tangent) into the direction V runs: cross(+Y, +X) is -Z, and V
/// runs along +Z, so w is -1. A frame with w the wrong way round lights every
/// normal-map bump from the opposite side, and looks plausible doing it.
void mikkTangentsOnQuad() {
    std::vector<Vertex> vertices{
        Vertex{.position = {0, 0, 0}, .normal = {0, 1, 0}, .uv = {0, 0}, .tangent = {}},
        Vertex{.position = {1, 0, 0}, .normal = {0, 1, 0}, .uv = {1, 0}, .tangent = {}},
        Vertex{.position = {1, 0, 1}, .normal = {0, 1, 0}, .uv = {1, 1}, .tangent = {}},
        Vertex{.position = {0, 0, 1}, .normal = {0, 1, 0}, .uv = {0, 1}, .tangent = {}},
    };
    // Counter-clockwise seen from above, so the faces face +Y like the normals.
    const std::vector<u32> indices{0, 2, 1, 0, 3, 2};

    computeTangents(vertices, indices);

    bool alongU = true;
    for (const Vertex& v : vertices) {
        alongU = alongU && near(v.tangent.x, 1.0f, 1e-4f) && near(v.tangent.y, 0.0f, 1e-4f) &&
                 near(v.tangent.z, 0.0f, 1e-4f) && v.tangent.w == -1.0f;
    }
    check(alongU, "quad tangents are (1, 0, 0) with w = -1");
}

/// MikkTSpace over a whole sphere: every tangent usable by a normal map.
void mikkTangentsOrthonormal() {
    const geometry::TriangleMesh triangles = geometry::makeUvSphere(1.0f, 16, 8).triangulate();

    std::vector<Vertex> vertices(triangles.positions.size());
    for (usize i = 0; i < vertices.size(); ++i) {
        vertices[i] = Vertex{triangles.positions[i], triangles.normals[i], triangles.uvs[i], Vec4{}};
    }
    computeTangents(vertices, triangles.indices);

    bool sound = !vertices.empty();
    for (const Vertex& v : vertices) {
        const Vec3 t{v.tangent.x, v.tangent.y, v.tangent.z};
        sound = sound && std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.z) &&
                near(length(t), 1.0f, 1e-3f) && std::abs(dot(t, v.normal)) <= 1e-3f &&
                (v.tangent.w == 1.0f || v.tangent.w == -1.0f);
    }
    check(sound, "sphere tangents are unit, orthogonal to the normal, and signed");
}

// --- GPU records -----------------------------------------------------------------

/// Which slot of the texture array a material's map is read from. Anything
/// that cannot be reached - no texture, a handle past the scene's textures, a
/// texture past the end of the array - reads the neutral one instead of a slot
/// nobody wrote.
void textureSlots() {
    check(textureSlot(TextureHandle{5}, 10, 0) == 5, "a texture in range keeps its slot");
    check(textureSlot(TextureHandle{}, 10, 7) == 7, "no texture reads the neutral slot");
    check(textureSlot(TextureHandle{12}, 10, 7) == 7, "a handle past the scene's textures reads the neutral slot");
    check(textureSlot(TextureHandle{1500}, 2000, 7) == 7, "a texture past the array's 1024 slots reads the neutral slot");
}

void recordFromMaterial() {
    const NeutralTextures neutral{.white = 5, .flatNormal = 6};

    Material material;
    material.emissiveColor = Vec3{1.0f, 0.5f, 0.0f};
    material.emissiveIntensity = 4.0f;
    const MaterialRecord glowing = toRecord(material, neutral, 10);
    check(glowing.emissive.x == 4.0f && glowing.emissive.y == 2.0f && glowing.emissive.z == 0.0f,
          "emissive is colour times intensity");
    check(glowing.normalTexture == neutral.flatNormal, "no normal map reads the flat normal");
    // glTF multiplies the emissive colour by the map. With no map the colour
    // alone has to come through, so the stand-in is white - black would turn
    // every emissive colour without a texture off.
    check(glowing.emissiveTexture == neutral.white, "no emissive map reads white, so the colour alone glows");
    check(glowing.baseColorTexture == neutral.white, "no colour map reads white");
    check(glowing.alphaCutoff < 0.0f, "an opaque material has no cutoff");

    material.alphaMode = AlphaMode::Mask;
    material.alphaCutoff = 0.3f;
    check(toRecord(material, neutral, 10).alphaCutoff == 0.3f, "a masked material carries its cutoff");

    material.baseColor = TextureHandle{4};
    check(toRecord(material, neutral, 10).baseColorTexture == 4, "a colour map reads its own slot");
}

// --- texture sets -----------------------------------------------------------------

using Paths = std::vector<std::filesystem::path>;

std::string name(const std::filesystem::path& p) { return p.filename().string(); }

/// ambientCG's naming: <Asset>_<resolution>-<format>_<Map>.<ext>, plus a
/// displacement map and a preview image that are not material maps at all.
void textureSetAmbientCg() {
    const Paths files{"Bricks059_2K-JPG_Color.jpg", "Bricks059_2K-JPG_NormalGL.jpg",
                      "Bricks059_2K-JPG_NormalDX.jpg", "Bricks059_2K-JPG_Roughness.jpg",
                      "Bricks059_2K-JPG_AmbientOcclusion.jpg", "Bricks059_2K-JPG_Displacement.jpg",
                      "Bricks059.png"};
    const TextureSetFiles set = findTextureSet(files);
    check(name(set.baseColor) == "Bricks059_2K-JPG_Color.jpg", "ambientCG: Color is the base colour");
    check(name(set.normal) == "Bricks059_2K-JPG_NormalGL.jpg" && !set.normalIsDirectX,
          "ambientCG: the OpenGL normal map wins over the DirectX one");
    check(name(set.roughness) == "Bricks059_2K-JPG_Roughness.jpg", "ambientCG: Roughness found");
    check(name(set.occlusion) == "Bricks059_2K-JPG_AmbientOcclusion.jpg", "ambientCG: AmbientOcclusion found");
    check(set.metallic.empty() && set.emissive.empty(), "ambientCG: no metalness or emission invented");
    check(set.usable(), "ambientCG: the set is usable");
}

/// Poly Haven's naming: <asset>_<map>_<resolution>.<ext>, the map possibly two
/// tokens (nor_dx), and only a DirectX normal map in this one.
void textureSetPolyHaven() {
    const Paths files{"brick_wall_diff_2k.jpg", "brick_wall_nor_dx_2k.jpg", "brick_wall_rough_2k.jpg",
                      "brick_wall_ao_2k.jpg"};
    const TextureSetFiles set = findTextureSet(files);
    check(name(set.baseColor) == "brick_wall_diff_2k.jpg", "Poly Haven: diff is the base colour");
    check(name(set.normal) == "brick_wall_nor_dx_2k.jpg" && set.normalIsDirectX,
          "Poly Haven: nor_dx is a normal map, flagged DirectX");
    check(name(set.roughness) == "brick_wall_rough_2k.jpg", "Poly Haven: rough found");
    check(name(set.occlusion) == "brick_wall_ao_2k.jpg", "Poly Haven: ao found");
}

void textureSetMixedCase() {
    const Paths files{"WOOD_COLOR.PNG", "wood_normal.png"};
    const TextureSetFiles set = findTextureSet(files);
    check(name(set.baseColor) == "WOOD_COLOR.PNG" && name(set.normal) == "wood_normal.png",
          "names match whatever their case");
}

/// A folder of anything else makes no material.
void textureSetUnusable() {
    const Paths files{"screenshot.png", "notes.txt", "wood_normalmap_notes.txt"};
    check(!findTextureSet(files).usable(), "a folder with no colour or normal map is not a material");
}

// --- packing ---------------------------------------------------------------------

Pixels solid(u32 width, u32 height, u8 value) {
    Pixels pixels{.width = width, .height = height, .rgba = {}};
    pixels.rgba.assign(static_cast<usize>(width) * height * 4, value);
    return pixels;
}

void packingSizes() {
    const Pixels roughness = solid(4, 4, 100);
    const Pixels metallic = solid(2, 2, 200);
    const Pixels packed = packMetallicRoughness(&roughness, &metallic);
    bool right = packed.width == 4 && packed.height == 4 && packed.rgba.size() == 4 * 4 * 4;
    for (usize i = 0; right && i < packed.rgba.size(); i += 4) {
        right = packed.rgba[i] == 255 && packed.rgba[i + 1] == 100 && packed.rgba[i + 2] == 200 &&
                packed.rgba[i + 3] == 255;
    }
    check(right, "roughness to G, metalness to B, at the larger size");

    const Pixels roughOnly = packMetallicRoughness(&roughness, nullptr);
    check(roughOnly.rgba.size() >= 4 && roughOnly.rgba[1] == 100 && roughOnly.rgba[2] == 255,
          "no metalness map leaves B at 255, for the factor to scale");
}

// --- .fmat files -----------------------------------------------------------------

std::filesystem::path scratch() {
    const auto dir = std::filesystem::temp_directory_path() / "fumar_material_tests";
    std::filesystem::create_directories(dir);
    return dir;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

void fmatRoundTrip() {
    const auto dir = scratch() / "round";
    std::filesystem::create_directories(dir);

    MaterialDesc desc;
    desc.name = "Round trip";
    desc.baseColor = dir / "c.png";
    desc.roughness = dir / "r.png";
    desc.metallic = dir / "m.png";
    desc.normal = dir / "n.png";
    desc.normalIsDirectX = true;
    desc.occlusion = dir / "ao.png";
    desc.emissive = dir / "e.png";
    desc.baseColorFactor = Vec4{0.5f, 0.25f, 0.125f, 0.75f};
    desc.metallicFactor = 0.5f;
    desc.roughnessFactor = 0.25f;
    desc.normalScale = 2.0f;
    desc.occlusionStrength = 0.5f;
    desc.emissiveColor = Vec3{1.0f, 0.5f, 0.0f};
    desc.emissiveIntensity = 4.0f;
    desc.alphaMode = AlphaMode::Mask;
    desc.alphaCutoff = 0.25f;
    desc.uvScale = Vec2{2.0f, 3.0f};

    const auto file = dir / "round.fmat";
    const bool written = writeMaterialFile(file, desc);
    const std::optional<MaterialDesc> read = readMaterialFile(file);
    check(written && read.has_value() && *read == desc, ".fmat written and read back unchanged");

    // A second, packed variant: the one-file metal-rough form.
    MaterialDesc packed;
    packed.name = "Packed";
    packed.packedMetallicRoughness = dir / "orm.png";
    const auto packedFile = dir / "packed.fmat";
    const auto packedBack = writeMaterialFile(packedFile, packed) ? readMaterialFile(packedFile) : std::nullopt;
    check(packedBack.has_value() && *packedBack == packed, "a packed metal-rough map survives the trip too");
}

void fmatMalformed() {
    const auto dir = scratch();
    writeText(dir / "notjson.fmat", "not json");
    writeText(dir / "future.fmat", R"({"fumarMaterial": 2})");
    writeText(dir / "badtype.fmat", R"({"fumarMaterial": 1, "uvScale": "big"})");
    writeText(dir / "noversion.fmat", R"({"name": "x"})");
    check(!readMaterialFile(dir / "notjson.fmat").has_value(), "a file that is not JSON is refused");
    check(!readMaterialFile(dir / "future.fmat").has_value(), "a newer format version is refused");
    check(!readMaterialFile(dir / "badtype.fmat").has_value(), "a field of the wrong type is refused");
    check(!readMaterialFile(dir / "noversion.fmat").has_value(), "JSON that is not a fumar material is refused");
    check(!readMaterialFile(dir / "missing.fmat").has_value(), "a missing file is refused");
}

/// A texture the file names but that is not there: the file still reads, with
/// the path made absolute - the GPU side is what warns and uses the neutral map.
void fmatMissingTexture() {
    const auto dir = scratch();
    writeText(dir / "gone.fmat", R"({"fumarMaterial": 1, "name": "Gone", "baseColor": {"texture": "gone.jpg"}})");
    const std::optional<MaterialDesc> read = readMaterialFile(dir / "gone.fmat");
    check(read.has_value() && read->baseColor == (dir / "gone.jpg").lexically_normal(),
          "a missing texture still reads, as an absolute path");
}

/// The header of a PNG 20000 pixels wide: larger than any GPU will make a
/// texture, and caught before anything decodes or allocates for it.
void hugeImageRefused() {
    const auto file = scratch() / "huge.png";
    const unsigned char header[] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,       // signature
        0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R',         // IHDR, 13 bytes
        0x00, 0x00, 0x4E, 0x20, 0x00, 0x00, 0x00, 0x01,     // 20000 x 1
        0x08, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 8-bit RGBA, CRC not checked
        // An empty IDAT: stb reads past IHDR looking for a tRNS chunk, and
        // stops - size known - at the first image data.
        0x00, 0x00, 0x00, 0x00, 'I', 'D', 'A', 'T', 0x00, 0x00, 0x00, 0x00,
    };
    std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(header), sizeof(header));
    const std::optional<ImageSize> size = readImageSize(file);
    check(size.has_value() && size->width == 20000 && size->height == 1, "an image's size is read from its header");
}

} // namespace

int main() {
    section("tangents");
    mikkTangentsOnQuad();
    mikkTangentsOrthonormal();

    section("gpu records");
    textureSlots();
    recordFromMaterial();

    section("texture sets");
    textureSetAmbientCg();
    textureSetPolyHaven();
    textureSetMixedCase();
    textureSetUnusable();
    packingSizes();
    hugeImageRefused();

    section("material files");
    fmatRoundTrip();
    fmatMalformed();
    fmatMissingTexture();

    std::printf(failures == 0 ? "\nall good\n" : "\n%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
