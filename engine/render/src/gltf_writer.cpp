#include "fumar/render/gltf_writer.hpp"

#include "fumar/core/log.hpp"
#include "fumar/render/mesh.hpp"

#include <nlohmann/json.hpp>

#include <cstring>
#include <fstream>
#include <vector>

namespace fumar {

namespace {

using Json = nlohmann::json;

// Numbers from the glTF 2.0 specification. Named here because the file is full
// of them and a bare 5126 explains nothing.
constexpr u32 kGlbMagic = 0x46546C67;     // "glTF"
constexpr u32 kGlbVersion = 2;
constexpr u32 kChunkJson = 0x4E4F534A;    // "JSON"
constexpr u32 kChunkBinary = 0x004E4942;  // "BIN\0"
constexpr u32 kComponentFloat = 5126;
constexpr u32 kComponentUnsignedInt = 5125;
constexpr u32 kTargetArrayBuffer = 34962;
constexpr u32 kTargetElementArrayBuffer = 34963;
constexpr u32 kModeTriangles = 4;

void append(std::vector<u8>& bytes, const void* data, usize size) {
    const auto* begin = static_cast<const u8*>(data);
    bytes.insert(bytes.end(), begin, begin + size);
}

void appendU32(std::vector<u8>& bytes, u32 value) {
    // glTF is little-endian, and so is every machine fumar runs on. A
    // big-endian port would byte-swap here.
    append(bytes, &value, sizeof(value));
}

} // namespace

bool writeGlb(const std::filesystem::path& path, const Mesh& mesh, const std::string& name) {
    const auto vertices = mesh.vertices();
    const auto indices = mesh.indices();
    if (vertices.empty() || indices.empty()) {
        FUMAR_WARN("not writing '{}': the mesh is empty", path.string());
        return false;
    }

    // --- the binary chunk: one tightly packed array per attribute ------------
    // Separate arrays rather than the interleaved Vertex layout, because that
    // is what every reader handles without thinking about strides. Each is a
    // multiple of four bytes long, so every array starts aligned, as the
    // specification requires.
    std::vector<u8> binary;
    const usize count = vertices.size();

    const usize positionOffset = binary.size();
    Vec3 low = vertices[0].position;
    Vec3 high = low;
    for (const Vertex& vertex : vertices) {
        append(binary, &vertex.position, sizeof(Vec3));
        low = min(low, vertex.position);
        high = max(high, vertex.position);
    }

    const usize normalOffset = binary.size();
    for (const Vertex& vertex : vertices) {
        append(binary, &vertex.normal, sizeof(Vec3));
    }

    const usize uvOffset = binary.size();
    for (const Vertex& vertex : vertices) {
        append(binary, &vertex.uv, sizeof(Vec2));
    }

    const usize indexOffset = binary.size();
    append(binary, indices.data(), indices.size_bytes());

    // --- the JSON chunk: what those bytes mean -------------------------------
    const auto bufferView = [&](usize offset, usize length, u32 target) {
        return Json{{"buffer", 0}, {"byteOffset", offset}, {"byteLength", length}, {"target", target}};
    };

    Json primitive{
        {"attributes", Json{{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}},
        {"indices", 3},
        {"mode", kModeTriangles},
    };

    // On the primitive rather than the mesh: the sizes describe this
    // primitive's triangles, and it is also where cgltf hands extras back
    // whole - for a mesh it keeps only the byte offsets.
    const auto polygonSizes = mesh.polygonSizes();
    if (!polygonSizes.empty()) {
        primitive["extras"] = Json{{"fumarPolygons", std::vector<u32>(polygonSizes.begin(), polygonSizes.end())}};
    }

    const Json meshJson{
        {"name", name},
        {"primitives", Json::array({primitive})},
    };

    const Json gltf{
        {"asset", Json{{"version", "2.0"}, {"generator", "fumar"}}},
        {"scene", 0},
        {"scenes", Json::array({Json{{"nodes", Json::array({0})}}})},
        {"nodes", Json::array({Json{{"mesh", 0}, {"name", name}}})},
        {"meshes", Json::array({meshJson})},
        {"buffers", Json::array({Json{{"byteLength", binary.size()}}})},
        {"bufferViews", Json::array({
                            bufferView(positionOffset, count * sizeof(Vec3), kTargetArrayBuffer),
                            bufferView(normalOffset, count * sizeof(Vec3), kTargetArrayBuffer),
                            bufferView(uvOffset, count * sizeof(Vec2), kTargetArrayBuffer),
                            bufferView(indexOffset, indices.size_bytes(), kTargetElementArrayBuffer),
                        })},
        {"accessors", Json::array({
                          // POSITION is the one attribute whose bounds are
                          // mandatory: readers cull and frame models with them.
                          Json{{"bufferView", 0},
                               {"componentType", kComponentFloat},
                               {"count", count},
                               {"type", "VEC3"},
                               {"min", Json::array({low.x, low.y, low.z})},
                               {"max", Json::array({high.x, high.y, high.z})}},
                          Json{{"bufferView", 1}, {"componentType", kComponentFloat}, {"count", count}, {"type", "VEC3"}},
                          Json{{"bufferView", 2}, {"componentType", kComponentFloat}, {"count", count}, {"type", "VEC2"}},
                          Json{{"bufferView", 3},
                               {"componentType", kComponentUnsignedInt},
                               {"count", indices.size()},
                               {"type", "SCALAR"}},
                      })},
    };

    // Both chunks have to be a multiple of four bytes long: JSON is padded
    // with spaces, which a JSON parser skips, and the binary with zeros.
    std::string json = gltf.dump();
    json.append((4 - json.size() % 4) % 4, ' ');
    binary.resize(binary.size() + (4 - binary.size() % 4) % 4, 0);

    std::vector<u8> file;
    appendU32(file, kGlbMagic);
    appendU32(file, kGlbVersion);
    appendU32(file, static_cast<u32>(12 + 8 + json.size() + 8 + binary.size()));
    appendU32(file, static_cast<u32>(json.size()));
    appendU32(file, kChunkJson);
    append(file, json.data(), json.size());
    appendU32(file, static_cast<u32>(binary.size()));
    appendU32(file, kChunkBinary);
    append(file, binary.data(), binary.size());

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    if (!out) {
        FUMAR_ERROR("could not write '{}'", path.string());
        return false;
    }

    FUMAR_INFO("wrote '{}': {} vertices, {} triangles", path.filename().string(), count, indices.size() / 3);
    return true;
}

} // namespace fumar
