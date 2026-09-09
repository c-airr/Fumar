#pragma once

#include "fumar/core/math.hpp"
#include "fumar/core/types.hpp"
#include "fumar/render/mesh.hpp"
#include "fumar/rhi/image.hpp"
#include "fumar/rhi/vk_common.hpp"

#include <filesystem>
#include <optional>
#include <vector>

namespace fumar {

namespace rhi {
class DescriptorPool;
class Device;
class UploadContext;
} // namespace rhi

/// Everything a Model needs to turn a file on disk into GPU resources.
///
/// Passed in rather than owned, because textures and descriptor sets come from
/// pools the renderer manages - a model that allocated its own would fragment
/// them and make lifetime harder to reason about.
struct ModelLoadContext {
    rhi::Device& device;
    rhi::UploadContext& upload;
    rhi::DescriptorPool& descriptorPool;
    vk::DescriptorSetLayout materialSetLayout;
    vk::Sampler sampler;

    /// Stands in for materials that declare no base colour texture. glTF allows
    /// that, and a null image view is not a legal descriptor.
    vk::ImageView fallbackTexture;
};

/// A loaded glTF scene.
///
/// glTF is the format worth supporting first: it is the only widely used one
/// designed for runtime rather than for authoring, so its buffers are already
/// laid out the way a GPU wants them and there is no scene graph to interpret
/// beyond node transforms.
///
/// Flattened on load: the node hierarchy is baked into one world transform per
/// primitive. That is the right shape for drawing, and the wrong shape for an
/// editor - M3 will keep the hierarchy so nodes can be moved independently.
class Model {
public:
    Model() = default;

    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;
    Model(Model&&) noexcept = default;
    Model& operator=(Model&&) noexcept = default;

    /// Parses a .gltf or .glb file. Returns nothing if the file is missing or
    /// malformed - a failed load is a normal outcome, not a crash.
    static std::optional<Model> loadGltf(const std::filesystem::path& path, const ModelLoadContext& context);

    /// Draws every primitive, binding each one's material as it goes.
    ///
    /// `rootTransform` is applied on top of the transforms baked in at load, so
    /// the same model can be placed anywhere without reloading it.
    void draw(vk::CommandBuffer cmd, vk::PipelineLayout layout, const Mat4& rootTransform) const;

    bool empty() const { return m_primitives.empty(); }

    usize primitiveCount() const { return m_primitives.size(); }

    usize textureCount() const { return m_textures.size(); }

private:
    struct Primitive {
        Mesh mesh;
        vk::DescriptorSet materialSet;
        Mat4 transform;
    };

    std::vector<rhi::Image> m_textures;
    std::vector<Primitive> m_primitives;
};

} // namespace fumar
