#include "fumar/render/texture_set.hpp"

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace fumar {
namespace {

enum class Slot : u8 { BaseColor, NormalGl, NormalDx, Roughness, Metallic, Occlusion, Emissive };

/// The suffixes, in priority order within each slot. Lowercase; matched against
/// a lowercased name.
constexpr std::array<std::pair<std::string_view, Slot>, 19> kSuffixes{{
    {"_color", Slot::BaseColor},
    {"_basecolor", Slot::BaseColor},
    {"_albedo", Slot::BaseColor},
    {"_diff", Slot::BaseColor},
    {"_diffuse", Slot::BaseColor},
    {"_normalgl", Slot::NormalGl},
    {"_nor_gl", Slot::NormalGl},
    {"_normal", Slot::NormalGl},
    {"_normaldx", Slot::NormalDx},
    {"_nor_dx", Slot::NormalDx},
    {"_roughness", Slot::Roughness},
    {"_rough", Slot::Roughness},
    {"_metalness", Slot::Metallic},
    {"_metallic", Slot::Metallic},
    {"_metal", Slot::Metallic},
    {"_ambientocclusion", Slot::Occlusion},
    {"_ao", Slot::Occlusion},
    {"_emission", Slot::Emissive},
    {"_emissive", Slot::Emissive},
}};

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool isImage(const std::filesystem::path& file) {
    const std::string extension = lower(file.extension().string());
    return extension == ".jpg" || extension == ".jpeg" || extension == ".png" || extension == ".tga";
}

/// Whether `suffix` appears in `stem` as a whole token: followed by the end of
/// the name or a separator, never by more letters.
bool hasSuffix(std::string_view stem, std::string_view suffix) {
    for (usize at = stem.find(suffix); at != std::string_view::npos; at = stem.find(suffix, at + 1)) {
        const usize end = at + suffix.size();
        if (end == stem.size() || stem[end] == '_' || stem[end] == '-' || stem[end] == '.') {
            return true;
        }
    }
    return false;
}

std::vector<u8> readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// R of a pixel at (u, v) in 0..1, bilinear.
u8 sampleRed(const Pixels& image, f32 u, f32 v) {
    const f32 x = std::clamp(u * static_cast<f32>(image.width) - 0.5f, 0.0f, static_cast<f32>(image.width - 1));
    const f32 y = std::clamp(v * static_cast<f32>(image.height) - 0.5f, 0.0f, static_cast<f32>(image.height - 1));
    const u32 x0 = static_cast<u32>(x);
    const u32 y0 = static_cast<u32>(y);
    const u32 x1 = std::min(x0 + 1, image.width - 1);
    const u32 y1 = std::min(y0 + 1, image.height - 1);
    const f32 fx = x - static_cast<f32>(x0);
    const f32 fy = y - static_cast<f32>(y0);
    const auto at = [&](u32 px, u32 py) {
        return static_cast<f32>(image.rgba[(static_cast<usize>(py) * image.width + px) * 4]);
    };
    const f32 top = at(x0, y0) * (1.0f - fx) + at(x1, y0) * fx;
    const f32 bottom = at(x0, y1) * (1.0f - fx) + at(x1, y1) * fx;
    return static_cast<u8>(std::clamp(top * (1.0f - fy) + bottom * fy + 0.5f, 0.0f, 255.0f));
}

} // namespace

TextureSetFiles findTextureSet(std::span<const std::filesystem::path> files) {
    TextureSetFiles set;
    // The suffix that claimed each slot, by its position in kSuffixes: a later
    // file with an earlier suffix still takes the slot over.
    std::array<usize, 7> claimedBy;
    claimedBy.fill(kSuffixes.size());

    for (const std::filesystem::path& file : files) {
        if (!isImage(file)) {
            continue;
        }
        const std::string stem = lower(file.stem().string());
        for (usize i = 0; i < kSuffixes.size(); ++i) {
            if (!hasSuffix(stem, kSuffixes[i].first)) {
                continue;
            }
            const auto slot = static_cast<usize>(kSuffixes[i].second);
            if (i < claimedBy[slot]) {
                claimedBy[slot] = i;
                switch (kSuffixes[i].second) {
                case Slot::BaseColor: set.baseColor = file; break;
                case Slot::NormalGl: set.normal = file; break;
                case Slot::NormalDx: break; // decided below, after the GL one had its chance
                case Slot::Roughness: set.roughness = file; break;
                case Slot::Metallic: set.metallic = file; break;
                case Slot::Occlusion: set.occlusion = file; break;
                case Slot::Emissive: set.emissive = file; break;
                }
            }
            break;
        }
    }

    // A DirectX normal map only when there is no OpenGL one: fumar's frame is
    // the OpenGL convention, and a flipped map is a second-best.
    if (set.normal.empty() && claimedBy[static_cast<usize>(Slot::NormalDx)] < kSuffixes.size()) {
        const usize i = claimedBy[static_cast<usize>(Slot::NormalDx)];
        for (const std::filesystem::path& file : files) {
            if (isImage(file) && hasSuffix(lower(file.stem().string()), kSuffixes[i].first)) {
                set.normal = file;
                set.normalIsDirectX = true;
                break;
            }
        }
    }
    return set;
}

TextureSetFiles findTextureSet(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (entry.is_regular_file(ec)) {
            files.push_back(entry.path());
        }
    }
    // Sorted, so which file wins a tie does not depend on the file system's
    // listing order.
    std::sort(files.begin(), files.end());
    return findTextureSet(files);
}

std::optional<Pixels> loadPixels(const std::filesystem::path& file) {
    // Read through the standard library rather than handed to stb by name:
    // stb opens files with fopen, which on Windows cannot open a path outside
    // the local code page.
    const std::vector<u8> bytes = readFile(file);
    if (bytes.empty()) {
        return std::nullopt;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* data = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels,
                                          STBI_rgb_alpha);
    if (data == nullptr) {
        return std::nullopt;
    }
    Pixels pixels{.width = static_cast<u32>(width), .height = static_cast<u32>(height), .rgba = {}};
    pixels.rgba.assign(data, data + static_cast<usize>(width) * static_cast<usize>(height) * 4);
    stbi_image_free(data);
    return pixels;
}

std::optional<ImageSize> readImageSize(const std::filesystem::path& file) {
    const std::vector<u8> bytes = readFile(file);
    int width = 0;
    int height = 0;
    int channels = 0;
    if (bytes.empty() ||
        stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels) == 0) {
        return std::nullopt;
    }
    return ImageSize{.width = static_cast<u32>(width), .height = static_cast<u32>(height)};
}

Pixels packMetallicRoughness(const Pixels* roughness, const Pixels* metallic) {
    const auto valid = [](const Pixels* p) { return p != nullptr && p->width > 0 && p->height > 0; };
    u32 width = 1;
    u32 height = 1;
    for (const Pixels* p : {roughness, metallic}) {
        if (valid(p) && static_cast<u64>(p->width) * p->height > static_cast<u64>(width) * height) {
            width = p->width;
            height = p->height;
        }
    }

    Pixels packed{.width = width, .height = height, .rgba = {}};
    packed.rgba.resize(static_cast<usize>(width) * height * 4);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(width);
            const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height);
            u8* out = &packed.rgba[(static_cast<usize>(y) * width + x) * 4];
            out[0] = 255;
            out[1] = valid(roughness) ? sampleRed(*roughness, u, v) : 255;
            out[2] = valid(metallic) ? sampleRed(*metallic, u, v) : 255;
            out[3] = 255;
        }
    }
    return packed;
}

bool writePng(const std::filesystem::path& file, const Pixels& pixels) {
    if (pixels.width == 0 || pixels.height == 0 || pixels.rgba.size() < static_cast<usize>(pixels.width) * pixels.height * 4) {
        return false;
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    // Through a callback for the same reason loadPixels reads the bytes itself.
    const auto write = [](void* context, void* data, int size) {
        static_cast<std::ofstream*>(context)->write(static_cast<const char*>(data), size);
    };
    const int written = stbi_write_png_to_func(write, &out, static_cast<int>(pixels.width),
                                               static_cast<int>(pixels.height), 4, pixels.rgba.data(),
                                               static_cast<int>(pixels.width * 4));
    out.close();
    return written != 0 && static_cast<bool>(out);
}

} // namespace fumar
