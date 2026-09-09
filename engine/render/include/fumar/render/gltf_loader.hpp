#pragma once

#include "fumar/core/types.hpp"
#include "fumar/render/resources.hpp"
#include "fumar/rhi/vk_common.hpp"
#include "fumar/scene/scene.hpp"

#include <filesystem>

namespace fumar {

namespace rhi {
class DescriptorPool;
class Device;
class UploadContext;
} // namespace rhi

/// Everything the loader needs to turn a file into GPU resources.
///
/// Passed in rather than owned, because textures, descriptor sets and buffers
/// all come from pools the renderer manages.
struct GltfLoadContext {
    rhi::Device& device;
    rhi::UploadContext& upload;
    rhi::DescriptorPool& descriptorPool;
    vk::DescriptorSetLayout materialSetLayout;
    vk::Sampler sampler;

    /// Stands in for materials with no base colour texture, which glTF allows.
    /// A null image view is not a legal descriptor.
    vk::ImageView fallbackTexture;
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
NodeId loadGltfIntoScene(const std::filesystem::path& path, const GltfLoadContext& context, Scene& scene,
                         ResourceRegistry& resources, NodeId parent = kRootNode);

} // namespace fumar
