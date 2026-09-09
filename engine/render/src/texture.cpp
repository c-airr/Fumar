#include "fumar/render/texture.hpp"

#include "fumar/core/log.hpp"
#include "fumar/rhi/device.hpp"
#include "fumar/rhi/upload_context.hpp"

#include <stb_image.h>

namespace fumar {
namespace {

/// Wraps decoded pixels so they are freed however the function exits.
///
/// stb_image hands back malloc'd memory that has to go to stbi_image_free, not
/// delete, so it cannot simply live in a unique_ptr with the default deleter.
struct StbPixels {
    stbi_uc* data = nullptr;
    int width = 0;
    int height = 0;

    ~StbPixels() {
        if (data != nullptr) {
            stbi_image_free(data);
        }
    }

    StbPixels() = default;
    StbPixels(const StbPixels&) = delete;
    StbPixels& operator=(const StbPixels&) = delete;

    usize byteSize() const { return static_cast<usize>(width) * static_cast<usize>(height) * 4; }
};

rhi::Image uploadPixels(rhi::Device& device, rhi::UploadContext& upload, const StbPixels& pixels,
                        bool srgb) {
    rhi::Image image(device, rhi::ImageDesc{
                                 .extent = vk::Extent2D{static_cast<u32>(pixels.width),
                                                        static_cast<u32>(pixels.height)},
                                 .format = srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm,
                                 .usage = vk::ImageUsageFlagBits::eSampled |
                                          vk::ImageUsageFlagBits::eTransferDst,
                                 .aspect = vk::ImageAspectFlagBits::eColor,
                             });

    upload.uploadImage(image, pixels.data, pixels.byteSize());
    return image;
}

} // namespace

rhi::Image loadTextureFromFile(rhi::Device& device, rhi::UploadContext& upload,
                               const std::filesystem::path& path, bool srgb) {
    StbPixels pixels;
    int channels = 0;

    // Forcing 4 channels even for opaque images: Vulkan support for three-channel
    // formats is optional and frequently missing, while RGBA8 is guaranteed.
    pixels.data = stbi_load(path.string().c_str(), &pixels.width, &pixels.height, &channels, STBI_rgb_alpha);

    if (pixels.data == nullptr) {
        FUMAR_ERROR("could not decode texture '{}': {}", path.string(), stbi_failure_reason());
        return {};
    }

    FUMAR_DEBUG("texture '{}': {}x{}, {} source channels", path.filename().string(), pixels.width,
                pixels.height, channels);
    return uploadPixels(device, upload, pixels, srgb);
}

rhi::Image loadTextureFromMemory(rhi::Device& device, rhi::UploadContext& upload, const void* data,
                                 usize bytes, bool srgb) {
    StbPixels pixels;
    int channels = 0;

    pixels.data = stbi_load_from_memory(static_cast<const stbi_uc*>(data), static_cast<int>(bytes),
                                        &pixels.width, &pixels.height, &channels, STBI_rgb_alpha);

    if (pixels.data == nullptr) {
        FUMAR_ERROR("could not decode embedded texture: {}", stbi_failure_reason());
        return {};
    }

    return uploadPixels(device, upload, pixels, srgb);
}

} // namespace fumar
