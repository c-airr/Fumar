#pragma once

#include "fumar/core/types.hpp"
#include "fumar/render/camera.hpp"
#include "fumar/render/mesh.hpp"
#include "fumar/render/resources.hpp"
#include "fumar/rhi/buffer.hpp"
#include "fumar/rhi/image.hpp"
#include "fumar/rhi/vk_common.hpp"
#include "fumar/scene/scene.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <memory>

namespace fumar {

class Window;

namespace rhi {
class DescriptorPool;
class Device;
class FrameContext;
class GraphicsPipeline;
class Instance;
class Swapchain;
class UploadContext;
} // namespace rhi

/// Owns the graphics device and draws a Scene with it.
///
/// The scene and its resources live here rather than in the application,
/// because both need the device to exist and to outlive them. Application code
/// reaches them through scene() and resources() and edits them directly - that
/// is the same surface the editor will use in M4.
class Renderer {
public:
    explicit Renderer(Window& window);

    /// Defined in the .cpp, because the unique_ptr members point at types that
    /// are only forward-declared here.
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    Camera& camera() { return m_camera; }
    const Camera& camera() const { return m_camera; }

    Scene& scene() { return m_scene; }
    const Scene& scene() const { return m_scene; }

    ResourceRegistry& resources() { return m_resources; }
    const ResourceRegistry& resources() const { return m_resources; }

    // --- content creation ---------------------------------------------------
    // Thin wrappers that hand the device and upload context to the factories,
    // so application code never has to hold either.

    MeshHandle createCubeMesh();
    MeshHandle createPlaneMesh(f32 halfSize, f32 uvTiling = 1.0f);

    /// Registers a material using a texture file, falling back to the built-in
    /// checkerboard if the file cannot be read.
    MaterialHandle createMaterial(std::string name, const std::filesystem::path& baseColorTexture = {});

    /// Loads a glTF file into the scene, keeping its node hierarchy. Returns
    /// the node it was rooted at, or kInvalidNode on failure.
    NodeId loadModel(const std::filesystem::path& path, NodeId parent = kRootNode);

    /// Records and submits one frame.
    void drawFrame();

    // --- UI integration -----------------------------------------------------
    // A user interface has to draw inside the same render pass as the scene,
    // after it, and its backend needs the raw Vulkan handles to set itself up.
    // These exist for that and nothing else.

    /// Recorded after the scene, still inside the render pass.
    using OverlayCallback = std::function<void(vk::CommandBuffer)>;
    void setOverlay(OverlayCallback overlay) { m_overlay = std::move(overlay); }

    rhi::Instance& instance() { return *m_instance; }
    rhi::Device& device() { return *m_device; }

    /// Colour format of the swapchain, which any pipeline drawing into it must
    /// be built for.
    vk::Format swapchainFormat() const;

    /// Format of the depth attachment.
    ///
    /// A UI drawing inside the scene's render pass needs this too: Vulkan
    /// requires every pipeline used in a render pass to declare exactly the
    /// attachment formats that pass has, depth included, even when the pipeline
    /// itself never touches depth.
    vk::Format depthFormat() const { return m_depthFormat; }

    u32 swapchainImageCount() const;

private:
    /// Returns false when the swapchain could not be rebuilt because the
    /// surface has no area yet - the window is minimised.
    bool recreateSwapchain();

    void createDepthBuffer();
    void createDefaultTexture();
    void createDescriptors();
    void updateCameraUniforms(u32 frameIndex);
    void recordCommands(u32 imageIndex);

    Window& m_window;

    std::unique_ptr<rhi::Instance> m_instance;

    /// Created by the window and handed back to it in the destructor. A plain
    /// handle rather than vk::UniqueSurfaceKHR, so that creation and
    /// destruction stay symmetrical - both go through Window, which is the
    /// thing that actually knows how the surface was made.
    vk::SurfaceKHR m_surface;

    std::unique_ptr<rhi::Device> m_device;
    std::unique_ptr<rhi::Swapchain> m_swapchain;
    std::unique_ptr<rhi::UploadContext> m_upload;
    std::unique_ptr<rhi::GraphicsPipeline> m_pipeline;
    std::unique_ptr<rhi::FrameContext> m_frames;
    std::unique_ptr<rhi::DescriptorPool> m_descriptorPool;

    vk::UniqueDescriptorSetLayout m_cameraSetLayout;
    vk::UniqueDescriptorSetLayout m_materialSetLayout;
    vk::UniqueSampler m_sampler;

    /// Depth attachment. Rebuilt with the swapchain, since it has to match the
    /// colour attachment pixel for pixel.
    rhi::Image m_depthImage;
    vk::Format m_depthFormat = vk::Format::eUndefined;

    /// Checkerboard used by materials with no texture of their own.
    rhi::Image m_defaultTexture;

    /// Descriptor set 0 plus its uniform buffer, duplicated per frame in
    /// flight. Writing to a single buffer while the GPU reads it for the
    /// previous frame would tear the camera between the two.
    struct PerFrame {
        rhi::Buffer cameraUniforms;
        vk::DescriptorSet cameraSet;
    };
    std::array<PerFrame, rhi::kFramesInFlight> m_perFrame;

    Scene m_scene;
    ResourceRegistry m_resources;
    Camera m_camera;
    OverlayCallback m_overlay;

    /// Set when the swapchain no longer matches the surface. Kept as state
    /// rather than handled on the spot, because a rebuild can fail (minimised
    /// window) and then has to be retried on a later frame.
    bool m_swapchainDirty = false;
};

} // namespace fumar
