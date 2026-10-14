#pragma once

#include "fumar/core/types.hpp"
#include "fumar/render/material_file.hpp"
#include "fumar/render/resources.hpp"
#include "fumar/rhi/vk_common.hpp"
#include "fumar/scene/scene.hpp"

#include <filesystem>

namespace fumar {

namespace rhi {
class Device;
class UploadContext;
} // namespace rhi

/// Everything the loader needs to turn a file into GPU resources.
///
/// Passed in rather than owned, because the device and the upload queue are
/// the renderer's. Nothing descriptor-related: materials are records the
/// renderer uploads itself, and textures reach the shaders through its array.
struct GltfLoadContext {
    rhi::Device& device;
    rhi::UploadContext& upload;
};

/// Loads a .gltf or .glb file into a scene, preserving its node hierarchy.
///
/// The hierarchy is kept rather than baked into world transforms, because that
/// is what an editor needs: moving a model's root has to carry its parts along,
/// and selecting a sub-part has to be possible. Meshes, textures and materials
/// are registered in `resources` and referenced from the scene by handle.
///
/// Returns the node the file was rooted at, or kInvalidNode on failure - a
/// failed load is a normal outcome, not a crash.
/// Writes the maps of material `index` of a glTF file out as PNG files in
/// `folder` - `<name>_Color.png` and so on - and points `desc` at them.
///
/// For making an editable copy of a material that lives inside a file: the
/// textures of a .glb are not files anyone can point a .fmat at until they
/// are. Factors in `desc` are left as they were. False if the file cannot be
/// read or a map cannot be written.
bool exportGltfMaterial(const std::filesystem::path& gltf, u32 index, const std::filesystem::path& folder,
                        MaterialDesc& desc);

NodeId loadGltfIntoScene(const std::filesystem::path& path, const GltfLoadContext& context, Scene& scene,
                         ResourceRegistry& resources, NodeId parent = kRootNode);

} // namespace fumar
