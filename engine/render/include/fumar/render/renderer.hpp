#pragma once

#include "fumar/core/types.hpp"
#include "fumar/render/camera.hpp"
#include "fumar/render/mesh.hpp"
#include "fumar/render/model.hpp"
#include "fumar/rhi/buffer.hpp"
#include "fumar/rhi/image.hpp"
#include "fumar/rhi/vk_common.hpp"

#include <array>
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

/// Ties the RHI objects together into something that draws.
///
/// Most members are unique_ptr, which looks heavier than it needs to be but
/// buys the one thing that matters here: explicit control over construction and
/// destruction order. Vulkan objects must be destroyed strictly inside the
/// lifetime of whatever created them, and getting that order wrong produces a
/// crash on exit rather than a compile error.
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

    /// Records and submits one frame.
    void drawFrame(f32 timeSeconds);

private:
    /// Returns false when the swapchain could not be rebuilt because the
    /// surface has no area yet - the window is minimised.
    bool recreateSwapchain();

    void createDepthBuffer();
    void createDefaultTexture();
    void createDescriptors();
    void loadSceneAssets();
    void updateCameraUniforms(u32 frameIndex);
    void recordCommands(u32 imageIndex, f32 timeSeconds);

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

    rhi::Image m_defaultTexture;

    /// Descriptor set 1: the material. One for now, shared by everything drawn.
    vk::DescriptorSet m_materialSet;

    /// Descriptor set 0 plus its uniform buffer, duplicated per frame in
    /// flight. Writing to a single buffer while the GPU reads it for the
    /// previous frame would tear the camera between the two.
    struct PerFrame {
        rhi::Buffer cameraUniforms;
        vk::DescriptorSet cameraSet;
    };
    std::array<PerFrame, rhi::kFramesInFlight> m_perFrame;

    Mesh m_cube;
    Mesh m_ground;

    /// Optional: the sandbox falls back to the procedural cubes when no model
    /// file is present, so the engine still runs on a fresh clone.
    Model m_model;

    Camera m_camera;

    /// Set when the swapchain no longer matches the surface. Kept as state
    /// rather than handled on the spot, because a rebuild can fail (minimised
    /// window) and then has to be retried on a later frame.
    bool m_swapchainDirty = false;
};

} // namespace fumar
