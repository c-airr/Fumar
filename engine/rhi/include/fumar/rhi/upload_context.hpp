#pragma once

#include "fumar/core/types.hpp"
#include "fumar/rhi/buffer.hpp"
#include "fumar/rhi/vk_common.hpp"

#include <functional>
#include <vector>

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
    ///
    /// Inside a batch (see UploadBatch) it only records: the submit and the
    /// wait happen once, when the batch ends. Each recording is fenced off
    /// from the one before by a full barrier, so the order still holds - a
    /// copy finishes before the structure built from what it copied.
    void immediateSubmit(const std::function<void(vk::CommandBuffer)>& record);

    /// Keeps a buffer alive until the work recorded so far has finished.
    ///
    /// Outside a batch that work is already done when immediateSubmit returns,
    /// and the buffer is simply released. Inside one, a staging or scratch
    /// buffer the recorded commands still read has to outlive the function
    /// that made it - until the batch's single wait.
    void retain(Buffer buffer);

    /// Starts and ends a batch. Prefer UploadBatch, which cannot forget the
    /// end. Batches nest; only the outermost one submits.
    void beginBatch();
    void endBatch();

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
    ///
    /// When the image was created with more than one mip level, the smaller
    /// ones are generated here from the pixels just uploaded.
    void uploadImage(Image& image, const void* pixels, usize bytes);

private:
    /// Fills levels 1..n by blitting each one from the level above, halved.
    ///
    /// Recorded into an already-open command buffer with level 0 in
    /// eTransferDstOptimal; leaves every level in eShaderReadOnlyOptimal.
    void generateMipmaps(vk::CommandBuffer cmd, Image& image);

    void begin();
    void submitAndWait();

    Device& m_device;
    vk::UniqueCommandPool m_pool;
    vk::CommandBuffer m_commandBuffer;
    vk::UniqueFence m_fence;

    u32 m_batchDepth = 0;
    bool m_batchRecorded = false;
    std::vector<Buffer> m_retained;
};

/// Everything uploaded while one of these is alive goes to the GPU in one
/// submit, with one wait at the end.
///
/// A mesh is three uploads - vertices, indices, and the acceleration structure
/// built from them - and each on its own is a submit and a full stop until the
/// GPU has caught up. In the Modeler a mesh is rebuilt every frame of a drag,
/// so the three stops were paid every frame.
class UploadBatch {
public:
    explicit UploadBatch(UploadContext& upload) : m_upload(upload) { m_upload.beginBatch(); }
    ~UploadBatch() { m_upload.endBatch(); }

    UploadBatch(const UploadBatch&) = delete;
    UploadBatch& operator=(const UploadBatch&) = delete;

private:
    UploadContext& m_upload;
};

} // namespace fumar::rhi
