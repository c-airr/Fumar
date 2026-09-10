#pragma once

#include "fumar/core/types.hpp"

#include <filesystem>

namespace fumar {

class Renderer;

/// Writes the scene, and everything needed to rebuild it, to a JSON file.
///
/// The scene itself is only half the story: its nodes reference meshes and
/// materials by handle, and a handle is a runtime index that means nothing in a
/// file. So the file also records how each resource was produced - a shape and
/// its parameters for generated meshes, a file and a primitive index for
/// imported ones, a colour and a texture path for materials. Loading replays
/// those descriptions and rebuilds the handles.
///
/// JSON rather than a binary format so a scene can be diffed in git and read by
/// a person. A shipping game would want the binary version; a project being
/// built does not.
bool saveScene(const Renderer& renderer, const std::filesystem::path& path);

/// Replaces the current scene and resources with the contents of a file.
///
/// On failure the renderer is left untouched rather than half-loaded: a broken
/// or missing file should cost you nothing.
bool loadScene(Renderer& renderer, const std::filesystem::path& path);

} // namespace fumar
