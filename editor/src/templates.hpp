#pragma once

#include "fumar/core/types.hpp"

#include <span>
#include <string_view>

namespace fumar {

class Renderer;

/// A scene the editor can start from: File > New scene lists these, the way
/// Unreal's project browser lists Blank, First Person and the rest.
///
/// A template is code, not a file. It builds into an empty renderer - nodes,
/// meshes, materials, the environment and the camera - and from then on the
/// result is an ordinary scene, saved and loaded like any other.
struct SceneTemplate {
    /// Stable name for scripts and smoke tests ("starter", "raytracing").
    const char* id;
    const char* name;
    const char* description;
    void (*build)(Renderer& renderer);
};

/// Every template, in the order the dialog shows them.
std::span<const SceneTemplate> sceneTemplates();

/// The one the editor opens with.
inline constexpr usize kDefaultSceneTemplate = 1;

/// Index of the template with this id, or kDefaultSceneTemplate if none has it.
usize findSceneTemplate(std::string_view id);

/// Builds template `index` into the renderer, which the caller has emptied
/// with resetScene. Starts from the default environment.
void buildSceneTemplate(Renderer& renderer, usize index);

} // namespace fumar
