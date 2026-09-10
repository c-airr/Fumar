#pragma once

#include "fumar/core/types.hpp"
#include "fumar/rhi/buffer.hpp"
#include "fumar/rhi/vk_common.hpp"

#include <functional>

namespace fumar::rhi {

class Device;
class Image;

/// One-shot GPU work that the CPU waits for.
///
/// Uploading a mesh or a texture means recording a copy and waiting for it,
/// which does not fit the per-frame command buffers at all - those are
/// pipelined and must never block. This keeps a separate command buffer and
/// fence for the blocking case.
///
/// Blocking is fine while loading; doing it per frame would serialise the CPU
/// and GPU completely. A streaming system would instead use a dedicated
/// transfer queue and check the fence on a later frame.
class UploadContext {
public:
    explicit UploadContext(Device& device);

    UploadContext(const UploadContext&) = delete;
    UploadContext& operator=(const UploadContext&) = delete;

    /// Records, submits, and waits for completion.
    void immediateSubmit(const std::function<void(vk::CommandBuffer)>& record);

    /// Creates a device-local buffer holding a copy of `data`.
    ///
    /// The bytes land in a host-visible staging buffer first and are then
    /// copied into VRAM, because the CPU cannot write device-local memory. The
    /// staging buffer is released as soon as the copy completes.
    Buffer createDeviceBuffer(const void* data, usize bytes, vk::BufferUsageFlags usage);

    /// The device this uploads to. Exposed because callers sometimes need to
    /// ask what it can do - whether to add ray tracing usage flags to a buffer,
    /// for instance - and threading a second reference through every call site
    /// would be worse.
    Device& device() { return m_device; }

    /// Fills an image from raw pixels and leaves it in eShaderReadOnlyOptimal,
    /// ready to sample.
    void uploadImage(Image& image, const void* pixels, usize bytes);

private:
    Device& m_device;
    vk::UniqueCommandPool m_pool;
    vk::CommandBuffer m_commandBuffer;
    vk::UniqueFence m_fence;
};

} // namespace fumar::rhi
