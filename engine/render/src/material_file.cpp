#include "fumar/render/material_file.hpp"

#include "fumar/core/log.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>
#include <system_error>

namespace fumar {
namespace {

using Json = nlohmann::json;

constexpr int kFormatVersion = 1;

/// Thrown inside the reader only, caught at its top: the shortest way out of a
/// file whose fields are the wrong shape, from however deep.
struct BadField {
    std::string what;
};

/// A path as the file stores it: relative to the file's folder where it can
/// be, absolute where it cannot (another drive). UTF-8, forward slashes.
Json pathToJson(const std::filesystem::path& path, const std::filesystem::path& folder) {
    if (path.empty()) {
        return nullptr;
    }
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(path, folder, ec);
    const std::u8string text = (ec || relative.empty() ? path : relative).generic_u8string();
    return std::string(text.begin(), text.end());
}

/// A path the file gives, made absolute.
///
/// A path to another machine is refused: opening \\server\share\x.png makes
/// Windows connect to that server and offer the user's credentials to it, and
/// a material downloaded from anywhere must not be able to make it do that.
std::filesystem::path pathFromJson(const Json& value, const std::filesystem::path& folder, const char* field) {
    if (value.is_null()) {
        return {};
    }
    if (!value.is_string()) {
        throw BadField{field};
    }
    const std::string text = value.get<std::string>();
    const auto separator = [](char c) { return c == '/' || c == '\\'; };
    if (text.size() >= 2 && separator(text[0]) && separator(text[1])) {
        throw BadField{std::string(field) + " (a network path)"};
    }
    const std::filesystem::path stored = pathFromUtf8(text);
    return (stored.is_absolute() ? stored : folder / stored).lexically_normal();
}

f32 number(const Json& object, const char* key, f32 fallback) {
    if (!object.contains(key)) {
        return fallback;
    }
    if (!object[key].is_number()) {
        throw BadField{key};
    }
    return object[key].get<f32>();
}

template <usize N>
std::array<f32, N> numbers(const Json& object, const char* key, std::array<f32, N> fallback) {
    if (!object.contains(key)) {
        return fallback;
    }
    const Json& value = object[key];
    if (!value.is_array() || value.size() != N) {
        throw BadField{key};
    }
    std::array<f32, N> out{};
    for (usize i = 0; i < N; ++i) {
        if (!value[i].is_number()) {
            throw BadField{key};
        }
        out[i] = value[i].get<f32>();
    }
    return out;
}

/// A sub-object, or an empty one where the file leaves it out.
const Json& section(const Json& root, const char* key) {
    static const Json empty = Json::object();
    if (!root.contains(key)) {
        return empty;
    }
    if (!root[key].is_object()) {
        throw BadField{key};
    }
    return root[key];
}

const Json& field(const Json& object, const char* key) {
    static const Json null = nullptr;
    return object.contains(key) ? object[key] : null;
}

} // namespace

std::string safeFileName(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        const bool reserved = byte < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                              c == '"' || c == '<' || c == '>' || c == '|';
        out.push_back(reserved ? '_' : c);
    }
    const auto trimmed = [](char c) { return c == '.' || c == ' '; };
    while (!out.empty() && trimmed(out.front())) {
        out.erase(out.begin());
    }
    while (!out.empty() && trimmed(out.back())) {
        out.pop_back();
    }
    return out.empty() ? "Material" : out;
}

std::string pathToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path pathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::optional<MaterialDesc> readMaterialFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        FUMAR_WARN("material file '{}' cannot be opened", pathToUtf8(file));
        return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // Without exceptions: a file that is not JSON is a warning, not a crash.
    const Json root = Json::parse(text, nullptr, false);
    if (!root.is_object() || !root.contains("fumarMaterial") || !root["fumarMaterial"].is_number_integer()) {
        FUMAR_WARN("'{}' is not a fumar material file", pathToUtf8(file));
        return std::nullopt;
    }
    if (root["fumarMaterial"].get<int>() != kFormatVersion) {
        FUMAR_WARN("'{}' is material format version {}, this build reads {}", pathToUtf8(file),
                   root["fumarMaterial"].get<int>(), kFormatVersion);
        return std::nullopt;
    }

    const std::filesystem::path folder = std::filesystem::absolute(file).parent_path();
    try {
        MaterialDesc desc;
        if (root.contains("name")) {
            if (!root["name"].is_string()) {
                throw BadField{"name"};
            }
            desc.name = root["name"].get<std::string>();
        } else {
            desc.name = pathToUtf8(file.stem());
        }

        const Json& base = section(root, "baseColor");
        desc.baseColor = pathFromJson(field(base, "texture"), folder, "baseColor.texture");
        const auto factor = numbers<4>(base, "factor", {1.0f, 1.0f, 1.0f, 1.0f});
        desc.baseColorFactor = Vec4{factor[0], factor[1], factor[2], factor[3]};

        const Json& mr = section(root, "metallicRoughness");
        desc.packedMetallicRoughness = pathFromJson(field(mr, "packed"), folder, "metallicRoughness.packed");
        desc.roughness = pathFromJson(field(mr, "roughness"), folder, "metallicRoughness.roughness");
        desc.metallic = pathFromJson(field(mr, "metallic"), folder, "metallicRoughness.metallic");
        desc.roughnessFactor = number(mr, "roughnessFactor", desc.roughnessFactor);
        desc.metallicFactor = number(mr, "metallicFactor", desc.metallicFactor);

        const Json& normal = section(root, "normal");
        desc.normal = pathFromJson(field(normal, "texture"), folder, "normal.texture");
        desc.normalScale = number(normal, "scale", desc.normalScale);
        if (normal.contains("convention")) {
            if (!normal["convention"].is_string()) {
                throw BadField{"normal.convention"};
            }
            desc.normalIsDirectX = normal["convention"].get<std::string>() == "dx";
        }

        const Json& occlusion = section(root, "occlusion");
        desc.occlusion = pathFromJson(field(occlusion, "texture"), folder, "occlusion.texture");
        desc.occlusionStrength = number(occlusion, "strength", desc.occlusionStrength);

        const Json& emissive = section(root, "emissive");
        desc.emissive = pathFromJson(field(emissive, "texture"), folder, "emissive.texture");
        const auto colour = numbers<3>(emissive, "color", {0.0f, 0.0f, 0.0f});
        desc.emissiveColor = Vec3{colour[0], colour[1], colour[2]};
        desc.emissiveIntensity = number(emissive, "intensity", desc.emissiveIntensity);

        const Json& alpha = section(root, "alpha");
        if (alpha.contains("mode")) {
            if (!alpha["mode"].is_string()) {
                throw BadField{"alpha.mode"};
            }
            desc.alphaMode = alpha["mode"].get<std::string>() == "mask" ? AlphaMode::Mask : AlphaMode::Opaque;
        }
        desc.alphaCutoff = number(alpha, "cutoff", desc.alphaCutoff);

        const auto uv = numbers<2>(root, "uvScale", {1.0f, 1.0f});
        desc.uvScale = Vec2{uv[0], uv[1]};
        return desc;
    } catch (const BadField& bad) {
        FUMAR_WARN("material file '{}': '{}' is not usable", pathToUtf8(file), bad.what);
        return std::nullopt;
    }
}

bool writeMaterialFile(const std::filesystem::path& file, const MaterialDesc& desc) {
    const std::filesystem::path folder = std::filesystem::absolute(file).parent_path();

    const Json root{
        {"fumarMaterial", kFormatVersion},
        {"name", desc.name},
        {"baseColor",
         Json{{"texture", pathToJson(desc.baseColor, folder)},
              {"factor", Json::array({desc.baseColorFactor.x, desc.baseColorFactor.y, desc.baseColorFactor.z,
                                      desc.baseColorFactor.w})}}},
        {"metallicRoughness",
         Json{{"packed", pathToJson(desc.packedMetallicRoughness, folder)},
              {"roughness", pathToJson(desc.roughness, folder)},
              {"metallic", pathToJson(desc.metallic, folder)},
              {"roughnessFactor", desc.roughnessFactor},
              {"metallicFactor", desc.metallicFactor}}},
        {"normal", Json{{"texture", pathToJson(desc.normal, folder)},
                        {"scale", desc.normalScale},
                        {"convention", desc.normalIsDirectX ? "dx" : "gl"}}},
        {"occlusion", Json{{"texture", pathToJson(desc.occlusion, folder)}, {"strength", desc.occlusionStrength}}},
        {"emissive",
         Json{{"texture", pathToJson(desc.emissive, folder)},
              {"color", Json::array({desc.emissiveColor.x, desc.emissiveColor.y, desc.emissiveColor.z})},
              {"intensity", desc.emissiveIntensity}}},
        {"alpha", Json{{"mode", desc.alphaMode == AlphaMode::Mask ? "mask" : "opaque"}, {"cutoff", desc.alphaCutoff}}},
        {"uvScale", Json::array({desc.uvScale.x, desc.uvScale.y})},
    };

    std::error_code ec;
    std::filesystem::create_directories(folder, ec);

    std::filesystem::path partial = file;
    partial += ".partial";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        if (!out) {
            FUMAR_ERROR("cannot write material file '{}'", pathToUtf8(file));
            return false;
        }
        // Invalid UTF-8 - a name typed in a code page - is replaced, not thrown:
        // a material that cannot be saved perfectly is still worth saving.
        out << root.dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
        out.close();
        if (!out) {
            FUMAR_ERROR("cannot write material file '{}': the write did not complete", pathToUtf8(file));
            std::filesystem::remove(partial, ec);
            return false;
        }
    }
    std::filesystem::rename(partial, file, ec);
    if (ec) {
        FUMAR_ERROR("cannot replace material file '{}': {}", pathToUtf8(file), ec.message());
        std::filesystem::remove(partial, ec);
        return false;
    }
    return true;
}

MaterialDesc describeMaterial(const Material& material) {
    return MaterialDesc{
        .name = material.name,
        .baseColor = pathFromUtf8(material.baseColorPath),
        .packedMetallicRoughness = pathFromUtf8(material.packedMetallicRoughnessPath),
        .metallic = pathFromUtf8(material.metallicPath),
        .roughness = pathFromUtf8(material.roughnessPath),
        .normal = pathFromUtf8(material.normalPath),
        .normalIsDirectX = material.normalIsDirectX,
        .occlusion = pathFromUtf8(material.occlusionPath),
        .emissive = pathFromUtf8(material.emissivePath),
        .baseColorFactor = material.baseColorFactor,
        .metallicFactor = material.metallic,
        .roughnessFactor = material.roughness,
        .normalScale = material.normalScale,
        .occlusionStrength = material.occlusionStrength,
        .emissiveColor = material.emissiveColor,
        .emissiveIntensity = material.emissiveIntensity,
        .alphaMode = material.alphaMode,
        .alphaCutoff = material.alphaCutoff,
        .uvScale = material.uvScale,
    };
}

} // namespace fumar
