#pragma once

#include "fumar/core/types.hpp"
#include "fumar/render/camera.hpp"
#include "fumar/render/environment.hpp"
#include "fumar/render/mesh.hpp"
#include "fumar/render/resources.hpp"
#include "fumar/rhi/acceleration_structure.hpp"
#include "fumar/rhi/buffer.hpp"
#include "fumar/rhi/image.hpp"
#include "fumar/rhi/vk_common.hpp"
#include "fumar/scene/scene.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

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

    /// The sun and sky. Edited in place - every field is read fresh each frame,
    /// so there is nothing to notify and no way for the two to fall out of step.
    Environment& environment() { return m_environment; }
    const Environment& environment() const { return m_environment; }

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

    /// Imports whatever a path points at.
    ///
    /// A model becomes nodes in the scene; an image becomes a material named
    /// after the file, ready to assign. Returns the node a model was rooted at,
    /// or kInvalidNode for anything else - including an image, which produces a
    /// material rather than geometry.
    NodeId importAsset(const std::filesystem::path& path);

    /// True for a file extension the importer recognises.
    static bool isImportable(const std::filesystem::path& path);

    /// Empties the scene and releases every mesh, texture and material.
    ///
    /// Also resets the descriptor pool, which frees the camera sets along with
    /// the material ones - so those are reallocated here too. Without that,
    /// loading a few scenes in a row would exhaust the pool.
    void resetScene();

    /// Records and submits one frame.
    void drawFrame();

    /// Whether the GPU can trace rays, which decides whether shadows and
    /// ambient occlusion exist at all. Exposed so the interface can say so
    /// rather than leaving the sliders looking broken.
    bool rayTracingSupported() const;

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

    /// Distance to the nearest geometry the ray hits, or a negative number for
    /// a miss.
    ///
    /// The same bounding-box test picking uses, answering a different question:
    /// picking wants to know WHICH object, this wants to know HOW FAR. Scripts
    /// use it to find the floor under a character, which is how walking works
    /// without a physics engine.
    ///
    /// Bounding boxes, not triangles - so a ray can land on the empty corner of
    /// a box around a sphere. Exact enough for standing on a block, not exact
    /// enough for a bullet.
    f32 raycast(const Ray& ray, f32 maxDistance = 1000.0f) const;

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

    /// Points the tone mapping pass at the current HDR image. Called whenever
    /// that image is rebuilt, which invalidates the descriptor written before.
    void updateTonemapDescriptor();
    void createDefaultTexture();
    void createDescriptors();
    void allocateDescriptorSets();
    void updateFrameUniforms(u32 frameIndex);

    /// Rebuilds this frame's picture of where everything is, for the rays to
    /// trace against. Recorded before the scene pass, into the same command
    /// buffer.
    void recordAccelerationStructure(vk::CommandBuffer cmd, u32 frameIndex);
    void recordSceneRendering(vk::CommandBuffer cmd);
    void recordTonemap(vk::CommandBuffer cmd);
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

    /// Fills the frame with sky before any geometry, from a fullscreen triangle
    /// and no vertex buffer at all.
    std::unique_ptr<rhi::GraphicsPipeline> m_skyPipeline;

    /// Reads the HDR scene back and writes the displayable image. The last
    /// thing that happens to the picture.
    std::unique_ptr<rhi::GraphicsPipeline> m_tonemapPipeline;

    /// Same geometry, rasterised as lines. Used to outline the hovered and
    /// selected objects without a second render target or a stencil pass.
    std::unique_ptr<rhi::GraphicsPipeline> m_outlinePipeline;
    std::unique_ptr<rhi::FrameContext> m_frames;
    std::unique_ptr<rhi::DescriptorPool> m_descriptorPool;

    vk::UniqueDescriptorSetLayout m_cameraSetLayout;
    vk::UniqueDescriptorSetLayout m_materialSetLayout;
    vk::UniqueSampler m_sampler;

    /// Where the scene is actually drawn: a floating-point image, so a sunlit
    /// surface can be worth 20 and a shadow 0.02 and both survive to the tone
    /// mapper. Sized to the viewport panel, not to the window.
    rhi::Image m_sceneHdr;
    rhi::Image m_depthImage;

    /// The tone mapped result, in a displayable format. This is the image the
    /// interface samples to show the viewport.
    rhi::Image m_sceneColor;

    /// Descriptor pointing at m_sceneHdr, for the tone mapping pass. Rewritten
    /// whenever the viewport is resized, since that replaces the image.
    vk::DescriptorSet m_tonemapSet;
    Extent2D m_viewportExtent{1280, 720};

    vk::Format m_depthFormat = vk::Format::eUndefined;

    /// Format of the off-screen colour target. Fixed rather than copied from
    /// the swapchain, so the scene pipeline never has to be rebuilt when the
    /// window's format changes.
    static constexpr vk::Format kSceneColorFormat = vk::Format::eR8G8B8A8Srgb;

    /// Half-precision float per channel: 16 bits covers roughly 0.00006 to
    /// 65504, which is far more range than 8-bit UNORM's 256 steps between 0
    /// and 1, at half the bandwidth of full floats. This is the standard choice
    /// for an HDR render target and the reason a bright sun does not simply
    /// clip on the way into memory.
    static constexpr vk::Format kSceneHdrFormat = vk::Format::eR16G16B16A16Sfloat;

    /// Checkerboard used by materials with no texture of their own.
    rhi::Image m_defaultTexture;

    /// A small box drawn as a wireframe wherever there is a light.
    ///
    /// A light has no geometry, so without this it is invisible in the viewport
    /// and there is nothing to click on to select it. Every editor draws
    /// something here for the same reason.
    Mesh m_lightMarker;

    /// Descriptor set 0 plus its uniform buffer, duplicated per frame in
    /// flight. Writing to a single buffer while the GPU reads it for the
    /// previous frame would tear the camera between the two.
    struct PerFrame {
        rhi::Buffer cameraUniforms;
        vk::DescriptorSet cameraSet;

        /// This frame's copy of the scene, as the ray tracing hardware sees it.
        /// One per frame in flight, because the GPU may still be tracing
        /// against the previous frame's while this one is rebuilt.
        rhi::TopLevelStructure topLevel;

        /// The handle currently written into cameraSet. Compared against the
        /// live one so the descriptor is only rewritten when the structure was
        /// actually reallocated, which is rare.
        vk::AccelerationStructureKHR writtenStructure;

        /// One record per instance in the top level structure, in the same
        /// order: where its geometry lives and what it is made of. A ray that
        /// hits instance N looks up entry N here, which is the only way it can
        /// find out what it hit.
        rhi::Buffer instanceData;
    };
    std::array<PerFrame, rhi::kFramesInFlight> m_perFrame;

    /// Rebuilt every frame from the scene. A member rather than a local so the
    /// allocation is reused instead of being made and freed sixty times a
    /// second.
    std::vector<vk::AccelerationStructureInstanceKHR> m_instances;

    /// One entry per instance, in the same order, matching InstanceRecord in
    /// shaders/raytrace.glsl exactly.
    ///
    /// Read through GL_EXT_scalar_block_layout, which is why the members can be
    /// packed like this instead of every one being padded out to sixteen bytes
    /// the way std140 would. A ray that lands on instance N reads entry N to
    /// find out what it hit - without it, a hit is a distance and nothing else.
    struct InstanceRecord {
        vk::DeviceAddress vertices;
        vk::DeviceAddress indices;
        Vec4 baseColor;
        f32 metallic;
        f32 roughness;
        f32 padding[2];
    };
    std::vector<InstanceRecord> m_instanceRecords;

    Scene m_scene;
    ResourceRegistry m_resources;
    Camera m_camera;
    Environment m_environment;
    OverlayCallback m_overlay;

    NodeId m_selected = kInvalidNode;
    NodeId m_highlighted = kInvalidNode;

    /// Set when the swapchain no longer matches the surface. Kept as state
    /// rather than handled on the spot, because a rebuild can fail (minimised
    /// window) and then has to be retried on a later frame.
    bool m_swapchainDirty = false;
};

} // namespace fumar
