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
    MeshHandle createCylinderMesh(f32 radius, f32 height, u32 segments = 32);

    /// Registers a material.
    ///
    /// With no texture the material is a flat colour: the shader multiplies its
    /// base colour by a 1x1 white texture, so the colour comes through
    /// unchanged and no branch is needed in the shader.
    MaterialHandle createMaterial(std::string name, Vec4 baseColor = Vec4{1.0f, 1.0f, 1.0f, 1.0f},
                                  const std::filesystem::path& baseColorTexture = {});

    /// Loads a glTF file into the scene, keeping its node hierarchy. Returns
    /// the node it was rooted at, or kInvalidNode on failure.
    NodeId loadModel(const std::filesystem::path& path, NodeId parent = kRootNode);

    /// Records and submits one frame.
    void drawFrame();

    /// Blocks until the GPU has finished everything submitted so far.
    ///
    /// Application code needs this exactly once, on shutdown, before releasing
    /// anything the last frame might still reference. Calling it per frame
    /// would throw away all the CPU/GPU overlap the frames-in-flight machinery
    /// exists to provide.
    void waitIdle() const;

    // --- UI integration -----------------------------------------------------
    // A user interface has to draw inside the same render pass as the scene,
    // after it, and its backend needs the raw Vulkan handles to set itself up.
    // These exist for that and nothing else.

    /// Recorded after the scene, still inside the render pass.
    using OverlayCallback = std::function<void(vk::CommandBuffer)>;
    void setOverlay(OverlayCallback overlay) { m_overlay = std::move(overlay); }

    rhi::Instance& instance() { return *m_instance; }
    rhi::Device& device() { return *m_device; }

    // --- viewport target ----------------------------------------------------
    // The scene is drawn into an off-screen image rather than straight to the
    // window, and the interface displays that image inside a panel. That is
    // what lets the viewport be a dockable panel of any size and position
    // instead of being whatever the window happens to be.

    /// Resizes the off-screen target. Ignored when the size is unchanged or
    /// degenerate, since rebuilding it stalls the GPU.
    ///
    /// Returns true if the target was actually rebuilt, which is the caller's
    /// cue that any descriptor pointing at the old image is now stale.
    bool resizeViewport(Extent2D size);

    vk::ImageView viewportImageView() const { return m_sceneColor.view(); }

    vk::Sampler viewportSampler() const { return *m_sampler; }

    Extent2D viewportExtent() const { return m_viewportExtent; }

    // --- selection feedback -------------------------------------------------
    // Drawn as a tint on the object itself rather than as an outline. An
    // outline needs either a stencil pass or an edge-detect filter over a
    // second render target; a tint is one float in the shader and reads just as
    // clearly on a solid-colour scene.

    /// The node shown as selected, or kInvalidNode for none.
    void setSelected(NodeId id) { m_selected = id; }
    NodeId selected() const { return m_selected; }

    /// The node under the cursor, tinted more faintly than the selection.
    void setHighlighted(NodeId id) { m_highlighted = id; }

    /// The closest node the ray hits, or kInvalidNode.
    ///
    /// Tests against each mesh bounding box, transformed into that object own
    /// space rather than transforming the box into the world - a rotated box
    /// is no longer axis-aligned, so testing it in the world would need the
    /// much larger box that contains it, and clicks would land on empty space
    /// beside thin rotated objects.
    NodeId pickNode(const Ray& ray) const;

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

    void createViewportTarget(Extent2D size);
    void createDefaultTexture();
    void createDescriptors();
    void updateCameraUniforms(u32 frameIndex);
    void recordSceneRendering(vk::CommandBuffer cmd);
    void recordUiRendering(vk::CommandBuffer cmd, u32 imageIndex);
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

    /// Same geometry, rasterised as lines. Used to outline the hovered and
    /// selected objects without a second render target or a stencil pass.
    std::unique_ptr<rhi::GraphicsPipeline> m_outlinePipeline;
    std::unique_ptr<rhi::FrameContext> m_frames;
    std::unique_ptr<rhi::DescriptorPool> m_descriptorPool;

    vk::UniqueDescriptorSetLayout m_cameraSetLayout;
    vk::UniqueDescriptorSetLayout m_materialSetLayout;
    vk::UniqueSampler m_sampler;

    /// The off-screen colour and depth the scene is drawn into. Sized to the
    /// viewport panel, not to the window.
    rhi::Image m_sceneColor;
    rhi::Image m_depthImage;
    Extent2D m_viewportExtent{1280, 720};

    vk::Format m_depthFormat = vk::Format::eUndefined;

    /// Format of the off-screen colour target. Fixed rather than copied from
    /// the swapchain, so the scene pipeline never has to be rebuilt when the
    /// window's format changes.
    static constexpr vk::Format kSceneColorFormat = vk::Format::eR8G8B8A8Srgb;

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

    NodeId m_selected = kInvalidNode;
    NodeId m_highlighted = kInvalidNode;

    /// Set when the swapchain no longer matches the surface. Kept as state
    /// rather than handled on the spot, because a rebuild can fail (minimised
    /// window) and then has to be retried on a later frame.
    bool m_swapchainDirty = false;
};

} // namespace fumar
