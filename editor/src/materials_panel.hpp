#pragma once

#include "fumar/scene/handles.hpp"

#include <filesystem>

namespace fumar {

class FileDialog;
class ImGuiLayer;
class Renderer;
class Window;
struct EditorState;

/// The Modeler's Materials panel: the library on the left, the material being
/// edited on the right.
///
/// The library lists the scene's materials, the stock ones under
/// assets/materials and the user's own under materials/ beside the executable.
/// Clicking one puts it on the selected object. The editor shows the selected
/// object's material, or the one last picked: its maps, its factors, live in
/// the viewport and with undo. Materials from a glTF or made in code are
/// read-only there - they belong to a file or to nobody - and a button makes
/// an editable .fmat copy.
void drawMaterialsPanel(EditorState& state, Renderer& renderer, ImGuiLayer& ui, FileDialog& dialog, Window& window);

/// Forgets every thumbnail and every handle the panel holds. For just before
/// the registry is replaced - a scene loaded or a new one started - and with
/// the GPU idle: a thumbnail is a descriptor pointing at a texture that is
/// about to be destroyed, and a handle would point at somebody else's material.
void resetMaterialsPanel(EditorState& state, ImGuiLayer& ui);

/// A new, empty material: `New Material.fmat` in `directory`, or `New
/// Material 2.fmat` and so on if that is taken. Never overwrites a file.
MaterialHandle newMaterial(Renderer& renderer, const std::filesystem::path& directory);

/// An editable copy of `source` as a .fmat in a folder of its own under
/// `directory`. A glTF material's maps are written out beside it as PNG files,
/// since inside a .glb they are not files anything can point at.
MaterialHandle duplicateAsFmat(Renderer& renderer, MaterialHandle source, const std::filesystem::path& directory);

} // namespace fumar
