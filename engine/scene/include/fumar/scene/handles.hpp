#pragma once

#include "fumar/core/types.hpp"

namespace fumar {

/// A typed index into a resource registry.
///
/// Scene nodes refer to meshes and materials by handle rather than by pointer,
/// for three reasons that all matter later:
///
///   * the scene stays free of GPU types, so it can be built, saved and edited
///     without a Vulkan device existing;
///   * a registry can move or reload its contents without leaving the scene
///     holding dangling pointers;
///   * a handle is 4 bytes and trivially serialisable, where a pointer is
///     neither.
///
/// The Tag parameter exists purely so a MeshHandle cannot be passed where a
/// MaterialHandle is expected. It is never defined - only its name is used.
template <typename Tag>
struct Handle {
    static constexpr u32 kInvalid = ~0u;

    u32 index = kInvalid;

    bool valid() const { return index != kInvalid; }

    friend constexpr bool operator==(const Handle&, const Handle&) = default;
};

struct MeshTag;
struct MaterialTag;
struct TextureTag;

using MeshHandle = Handle<MeshTag>;
using MaterialHandle = Handle<MaterialTag>;
using TextureHandle = Handle<TextureTag>;

} // namespace fumar
