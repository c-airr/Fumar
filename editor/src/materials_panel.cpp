#include "materials_panel.hpp"

#include "panels.hpp"

#include "fumar/core/log.hpp"
#include "fumar/platform/file_dialog.hpp"
#include "fumar/platform/paths.hpp"
#include "fumar/render/gltf_loader.hpp"
#include "fumar/render/material_file.hpp"
#include "fumar/render/renderer.hpp"
#include "fumar/ui/imgui_layer.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

namespace fumar {
namespace {

constexpr f32 kThumbnail = 40.0f;

/// The user's own materials, beside the executable - outside anything a build
/// copies over.
std::filesystem::path userDirectory() { return executableDirectory() / "materials"; }
std::filesystem::path stockDirectory() { return stockMaterialsDirectory(); }

/// A name made safe to be a file name: anything a file system might object to
/// becomes '_'.
std::string fileNameFor(const std::string& name) {
    std::string out;
    for (const char c : name) {
        const bool safe = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == ' ' || c == '-' || c == '_';
        out.push_back(safe ? c : '_');
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out.empty() ? "Material" : out;
}

/// `base`, or `base 2`, `base 3`... - whichever is not taken in `directory`,
/// as a file with `extension` or, with no extension, as a folder.
std::filesystem::path unusedPath(const std::filesystem::path& directory, const std::string& base,
                                 const std::string& extension) {
    std::error_code ec;
    for (u32 n = 1;; ++n) {
        const std::string name = n == 1 ? base : base + " " + std::to_string(n);
        const std::filesystem::path candidate = directory / (name + extension);
        if (!std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
}

bool contains(const std::string& text, const std::string& filter) {
    if (filter.empty()) {
        return true;
    }
    const auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    return lower(text).find(lower(filter)) != std::string::npos;
}

/// A texture as ImGui can draw it, registered the first time it is asked for.
ImTextureID thumbnail(EditorState& state, ImGuiLayer& ui, const Renderer& renderer, TextureHandle texture) {
    if (!renderer.resources().has(texture)) {
        return 0;
    }
    if (const auto found = state.materialThumbnails.find(texture.index); found != state.materialThumbnails.end()) {
        return found->second;
    }
    const ImTextureID id =
        ui.registerTexture(renderer.resources().texture(texture).view(), renderer.viewportSampler());
    state.materialThumbnails.emplace(texture.index, id);
    return id;
}

/// A square showing a material: its colour map, or its colour.
bool swatch(EditorState& state, ImGuiLayer& ui, const Renderer& renderer, const Material& material, const char* id,
            f32 size) {
    const ImTextureID image = thumbnail(state, ui, renderer, material.baseColor);
    if (image != 0) {
        const Vec4 tint = material.baseColorFactor;
        return ImGui::ImageButton(id, image, ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0),
                                  ImVec4(tint.x, tint.y, tint.z, 1.0f));
    }
    const Vec4 c = material.baseColorFactor;
    return ImGui::ColorButton(id, ImVec4(c.x, c.y, c.z, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(size, size));
}

bool hasTarget(const EditorState& state, const Renderer& renderer) {
    const Scene& scene = renderer.scene();
    return state.selected != kInvalidNode && scene.isAlive(state.selected) &&
           renderer.resources().has(scene.node(state.selected).mesh);
}

/// Puts a material on the selected object, as a step of the history.
void assign(EditorState& state, Renderer& renderer, MaterialHandle material) {
    if (!material.valid() || !hasTarget(state, renderer)) {
        return;
    }
    Scene& scene = renderer.scene();
    if (scene.node(state.selected).material == material) {
        return;
    }
    state.history.record(scene, state.selected);
    scene.node(state.selected).material = material;
}

void scanFiles(EditorState& state) {
    state.materialFiles.clear();
    std::error_code ec;
    for (const std::filesystem::path& root : {stockDirectory(), userDirectory()}) {
        if (!std::filesystem::exists(root, ec)) {
            continue;
        }
        for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().extension() == ".fmat") {
                state.materialFiles.push_back(std::filesystem::weakly_canonical(it->path(), ec));
            }
        }
    }
    std::sort(state.materialFiles.begin(), state.materialFiles.end());
    state.materialFilesScanned = true;
}

// --- the library -------------------------------------------------------------------

void drawLibrary(EditorState& state, Renderer& renderer, ImGuiLayer& ui) {
    ResourceRegistry& resources = renderer.resources();

    char filter[128];
    std::snprintf(filter, sizeof(filter), "%s", state.materialFilter.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##filter", "filter", filter, sizeof(filter))) {
        state.materialFilter = filter;
    }

    if (ImGui::Button("New")) {
        const MaterialHandle made = newMaterial(renderer, userDirectory());
        if (made.valid()) {
            state.editedMaterial = made;
            assign(state, renderer, made);
            scanFiles(state);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("A new material, saved as a .fmat in materials/ beside the editor.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan") || !state.materialFilesScanned) {
        scanFiles(state);
    }

    std::error_code ec;
    if (!std::filesystem::exists(stockDirectory(), ec)) {
        ImGui::TextWrapped("Stock materials are on the 'materials' branch - run tools/get-materials to fetch them.");
    }
    ImGui::Separator();

    const MaterialHandle current =
        hasTarget(state, renderer) ? renderer.scene().node(state.selected).material : MaterialHandle{};

    ImGui::BeginChild("##library");

    // The scene's materials, then the files not loaded yet.
    std::vector<std::string> loadedFiles;
    for (u32 i = 0; i < static_cast<u32>(resources.materialCount()); ++i) {
        const MaterialHandle handle{i};
        const Material& material = resources.material(handle);
        if (material.origin.kind == MaterialOrigin::Kind::Fmat) {
            loadedFiles.push_back(material.origin.file);
        }
        if (!contains(material.name, state.materialFilter)) {
            continue;
        }
        ImGui::PushID(static_cast<int>(i));
        if (swatch(state, ui, renderer, material, "##swatch", kThumbnail)) {
            state.editedMaterial = handle;
            assign(state, renderer, handle);
        }
        if (current == handle) {
            ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                ImGui::GetColorU32(ImGuiCol_CheckMark), 3.0f, 0, 2.0f);
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(material.name.empty() ? "(unnamed)" : material.name.c_str());
        const char* kind = material.origin.kind == MaterialOrigin::Kind::Fmat   ? ".fmat"
                           : material.origin.kind == MaterialOrigin::Kind::Gltf ? "glTF, read-only"
                                                                                : "built in, read-only";
        ImGui::TextDisabled("%s", kind);
        ImGui::EndGroup();
        ImGui::PopID();
    }

    for (const std::filesystem::path& file : state.materialFiles) {
        if (std::find(loadedFiles.begin(), loadedFiles.end(), pathToUtf8(file)) != loadedFiles.end() ||
            !contains(pathToUtf8(file.stem()), state.materialFilter)) {
            continue;
        }
        ImGui::PushID(pathToUtf8(file).c_str());
        if (ImGui::Button("load", ImVec2(kThumbnail, kThumbnail))) {
            const MaterialHandle loaded = renderer.loadMaterialFile(file);
            if (loaded.valid()) {
                state.editedMaterial = loaded;
                assign(state, renderer, loaded);
            }
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(pathToUtf8(file.stem()).c_str());
        ImGui::TextDisabled("not loaded yet");
        ImGui::EndGroup();
        ImGui::PopID();
    }

    ImGui::EndChild();
}

// --- the editor ----------------------------------------------------------------------

enum class MapSlot : int { Color, MetalRough, Normal, Occlusion, Emission };

/// The description with one map replaced (or cleared, with an empty path).
MaterialDesc withMap(MaterialDesc desc, MapSlot slot, const std::filesystem::path& file) {
    switch (slot) {
    case MapSlot::Color: desc.baseColor = file; break;
    case MapSlot::MetalRough:
        // A file picked here is taken as already packed - glTF's layout.
        desc.packedMetallicRoughness = file;
        desc.roughness.clear();
        desc.metallic.clear();
        break;
    case MapSlot::Normal: desc.normal = file; break;
    case MapSlot::Occlusion: desc.occlusion = file; break;
    case MapSlot::Emission: desc.emissive = file; break;
    }
    return desc;
}

/// Swaps a map, as a step of the history.
void replaceMap(EditorState& state, Renderer& renderer, MaterialHandle handle, const MaterialDesc& desc) {
    state.history.record(renderer.scene(), state.selected);
    renderer.rebuildMaterial(handle, desc);
    state.modifiedMaterials.insert(handle.index);
}

void drawMapSlot(EditorState& state, Renderer& renderer, ImGuiLayer& ui, FileDialog& dialog, Window& window,
                 MaterialHandle handle, MapSlot slot, const char* label, TextureHandle texture,
                 const std::string& path, bool readOnly) {
    ImGui::PushID(static_cast<int>(slot));
    const ImTextureID image = thumbnail(state, ui, renderer, texture);
    if (image != 0) {
        ImGui::Image(image, ImVec2(kThumbnail, kThumbnail));
    } else {
        ImGui::Dummy(ImVec2(kThumbnail, kThumbnail));
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                            ImGui::GetColorU32(ImGuiCol_Border));
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted(label);
    const std::string shown = !path.empty()          ? pathToUtf8(pathFromUtf8(path).filename())
                              : texture.valid()      ? "(inside the file)"
                                                     : "-";
    ImGui::TextDisabled("%s", shown.c_str());
    ImGui::EndGroup();

    if (!readOnly) {
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f + ImGui::GetCursorPosX());
        ImGui::BeginDisabled(dialog.pending());
        if (ImGui::Button("...")) {
            static constexpr std::array<FileFilter, 1> kImages{{{"Images", "png;jpg;jpeg;tga"}}};
            const Material& material = renderer.resources().material(handle);
            dialog.openFile(window, kImages, pathFromUtf8(material.origin.file).parent_path());
            state.pendingMapSlot = static_cast<int>(slot);
            state.pendingMapMaterial = handle;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(path.empty() && !texture.valid());
        if (ImGui::Button("x")) {
            replaceMap(state, renderer, handle,
                       withMap(describeMaterial(renderer.resources().material(handle)), slot, {}));
        }
        ImGui::EndDisabled();
    }
    ImGui::PopID();
}

void drawEditor(EditorState& state, Renderer& renderer, ImGuiLayer& ui, FileDialog& dialog, Window& window) {
    ResourceRegistry& resources = renderer.resources();
    Scene& scene = renderer.scene();

    // The selected object's material, whenever the selection changes; between
    // changes, whatever was last picked in the library.
    if (state.selected != state.materialPanelSelection) {
        state.materialPanelSelection = state.selected;
        if (hasTarget(state, renderer) && resources.has(scene.node(state.selected).material)) {
            state.editedMaterial = scene.node(state.selected).material;
        }
    }

    if (!resources.has(state.editedMaterial)) {
        ImGui::TextDisabled("Select an object, or pick a material on the left.");
        return;
    }

    const MaterialHandle handle = state.editedMaterial;
    Material& material = resources.material(handle);
    const bool readOnly = material.origin.kind != MaterialOrigin::Kind::Fmat;

    ImGui::TextUnformatted(material.name.c_str());
    if (readOnly) {
        ImGui::TextDisabled(material.origin.kind == MaterialOrigin::Kind::Gltf ? "Inside a glTF file: read-only."
                                                                             : "Built in: read-only.");
        if (ImGui::Button("Duplicate as .fmat")) {
            const MaterialHandle copy = duplicateAsFmat(renderer, handle, userDirectory());
            if (copy.valid()) {
                state.editedMaterial = copy;
                assign(state, renderer, copy);
                scanFiles(state);
            }
            return;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("An editable copy, saved in materials/ beside the editor,\n"
                              "with the maps written out as PNG files.");
        }
    } else {
        ImGui::TextDisabled("%s", pathToUtf8(pathFromUtf8(material.origin.file).filename()).c_str());
    }
    ImGui::Separator();

    // --- maps ---------------------------------------------------------------------
    drawMapSlot(state, renderer, ui, dialog, window, handle, MapSlot::Color, "Colour", material.baseColor,
                material.baseColorPath, readOnly);
    const std::string& metalRoughPath =
        !material.packedMetallicRoughnessPath.empty() ? material.packedMetallicRoughnessPath : material.roughnessPath;
    drawMapSlot(state, renderer, ui, dialog, window, handle, MapSlot::MetalRough, "Metal-rough (G/B)",
                material.metallicRoughness, metalRoughPath, readOnly);
    drawMapSlot(state, renderer, ui, dialog, window, handle, MapSlot::Normal, "Normal", material.normal,
                material.normalPath, readOnly);
    if (!readOnly && !material.normalPath.empty()) {
        bool directX = material.normalIsDirectX;
        if (ImGui::Checkbox("DirectX normal map (green down)", &directX)) {
            MaterialDesc desc = describeMaterial(material);
            desc.normalIsDirectX = directX;
            replaceMap(state, renderer, handle, desc);
        }
    }
    drawMapSlot(state, renderer, ui, dialog, window, handle, MapSlot::Occlusion, "Occlusion", material.occlusion,
                material.occlusionPath, readOnly);
    drawMapSlot(state, renderer, ui, dialog, window, handle, MapSlot::Emission, "Emission", material.emissive,
                material.emissivePath, readOnly);
    ImGui::Separator();

    // --- factors ------------------------------------------------------------------
    // Captured before any widget touches it: ImGui writes a clicked value on
    // the very frame a slider becomes active, so the material as it was has to
    // be kept from before the widgets ran, not read after.
    const Material before = material;
    bool changed = false;

    ImGui::BeginDisabled(readOnly);
    ImGui::PushItemWidth(-130.0f);
    changed |= ImGui::ColorEdit4("Base colour", &material.baseColorFactor.x, ImGuiColorEditFlags_Float);
    changed |= ImGui::SliderFloat("Metallic", &material.metallic, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Roughness", &material.roughness, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Normal strength", &material.normalScale, 0.0f, 2.0f);
    changed |= ImGui::SliderFloat("Occlusion", &material.occlusionStrength, 0.0f, 1.0f);
    changed |= ImGui::ColorEdit3("Emission", &material.emissiveColor.x, ImGuiColorEditFlags_Float);
    changed |= ImGui::DragFloat("Emission strength", &material.emissiveIntensity, 0.05f, 0.0f, 1000.0f);
    changed |= ImGui::DragFloat2("Tiling", &material.uvScale.x, 0.05f, 0.01f, 256.0f);
    int alphaMode = material.alphaMode == AlphaMode::Mask ? 1 : 0;
    if (ImGui::Combo("Alpha", &alphaMode, "Opaque\0Cutout\0")) {
        material.alphaMode = alphaMode == 1 ? AlphaMode::Mask : AlphaMode::Opaque;
        changed = true;
    }
    if (material.alphaMode == AlphaMode::Mask) {
        changed |= ImGui::SliderFloat("Cutoff", &material.alphaCutoff, 0.0f, 1.0f);
    }
    ImGui::PopItemWidth();
    ImGui::EndDisabled();

    if (changed) {
        // One undo step per gesture - a whole drag of a slider, a whole visit to
        // a colour picker - recorded with the value from before it began.
        if (!state.materialGestureOpen) {
            const Material now = material;
            material = before;
            state.history.record(scene, state.selected);
            material = now;
            state.materialGestureOpen = true;
        }
        renderer.markMaterialDirty(handle);
        state.modifiedMaterials.insert(handle.index);
    }
    if (!ImGui::IsAnyItemActive()) {
        state.materialGestureOpen = false;
    }

    if (!readOnly && state.modifiedMaterials.contains(handle.index)) {
        ImGui::Spacing();
        ImGui::TextDisabled("Unsaved - Ctrl+S saves it with the scene.");
    }
}

} // namespace

void drawMaterialsPanel(EditorState& state, Renderer& renderer, ImGuiLayer& ui, FileDialog& dialog, Window& window) {
    // A map picked in the system dialog arrives on a later frame than the
    // click that asked for it.
    if (std::optional<std::filesystem::path> chosen = dialog.takeResult();
        chosen && state.pendingMapSlot >= 0 && renderer.resources().has(state.pendingMapMaterial)) {
        const MaterialHandle handle = state.pendingMapMaterial;
        replaceMap(state, renderer, handle,
                   withMap(describeMaterial(renderer.resources().material(handle)),
                           static_cast<MapSlot>(state.pendingMapSlot), *chosen));
        state.pendingMapSlot = -1;
    }
    if (!dialog.pending()) {
        state.pendingMapSlot = -1;
    }

    if (const ImGuiWindow* content = ImGui::FindWindowByName("Content"); content != nullptr && content->DockId != 0) {
        ImGui::SetNextWindowDockID(content->DockId, ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("Materials")) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTable("##materials", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("library", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("editor", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        drawLibrary(state, renderer, ui);
        ImGui::TableNextColumn();
        ImGui::BeginChild("##editor");
        drawEditor(state, renderer, ui, dialog, window);
        ImGui::EndChild();
        ImGui::EndTable();
    }

    ImGui::End();
}

std::filesystem::path stockMaterialsDirectory() {
#if defined(FUMAR_MATERIALS_SOURCE_DIR)
    std::error_code ec;
    if (std::filesystem::exists(FUMAR_MATERIALS_SOURCE_DIR, ec)) {
        return FUMAR_MATERIALS_SOURCE_DIR;
    }
#endif
    return executableDirectory() / "assets" / "materials";
}

void resetMaterialsPanel(EditorState& state, ImGuiLayer& ui) {
    for (const auto& [index, id] : state.materialThumbnails) {
        ui.unregisterTexture(id);
    }
    state.materialThumbnails.clear();
    state.editedMaterial = MaterialHandle{};
    state.pendingMapSlot = -1;
    state.pendingMapMaterial = MaterialHandle{};
    state.modifiedMaterials.clear();
    state.materialGestureOpen = false;
    state.materialPanelSelection = kInvalidNode;
}

MaterialHandle newMaterial(Renderer& renderer, const std::filesystem::path& directory) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    const std::filesystem::path file = unusedPath(directory, "New Material", ".fmat");
    MaterialDesc desc;
    desc.name = pathToUtf8(file.stem());
    desc.roughnessFactor = 0.6f;
    if (!writeMaterialFile(file, desc)) {
        return {};
    }
    return renderer.loadMaterialFile(file);
}

MaterialHandle duplicateAsFmat(Renderer& renderer, MaterialHandle source, const std::filesystem::path& directory) {
    if (!renderer.resources().has(source)) {
        return {};
    }
    const Material& material = renderer.resources().material(source);
    MaterialDesc desc = describeMaterial(material);

    // A folder of its own: the maps written out for a glTF material need
    // somewhere to sit beside the file that names them.
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    const std::filesystem::path folder = unusedPath(directory, fileNameFor(material.name), "");
    std::filesystem::create_directories(folder, ec);
    desc.name = pathToUtf8(folder.filename());

    if (material.origin.kind == MaterialOrigin::Kind::Gltf &&
        !exportGltfMaterial(material.origin.file, material.origin.index, folder, desc)) {
        FUMAR_WARN("'{}' copied without some of its maps", material.name);
    }

    const std::filesystem::path file = folder / pathFromUtf8(desc.name + ".fmat");
    if (!writeMaterialFile(file, desc)) {
        return {};
    }
    FUMAR_INFO("material '{}' copied to '{}'", material.name, pathToUtf8(file));
    return renderer.loadMaterialFile(file);
}

} // namespace fumar
