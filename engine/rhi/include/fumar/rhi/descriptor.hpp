#pragma once

#include "fumar/core/types.hpp"
#include "fumar/rhi/vk_common.hpp"

#include <deque>
#include <span>
#include <vector>

namespace fumar::rhi {

class Device;

/// Fluent builder for a descriptor set layout.
///
/// A descriptor is a handle the shader uses to reach a resource - a uniform
/// buffer, a sampled texture. The *layout* is the declaration of which slots
/// exist and what type each one holds; it has to match the `layout(set, binding)`
/// declarations in the GLSL exactly, and nothing checks that for you.
class DescriptorSetLayoutBuilder {
public:
    DescriptorSetLayoutBuilder& binding(u32 index, vk::DescriptorType type, vk::ShaderStageFlags stages,
                                        u32 count = 1);

    vk::UniqueDescriptorSetLayout build(vk::Device device) const;

private:
    std::vector<vk::DescriptorSetLayoutBinding> m_bindings;
};

/// Pre-allocated storage that descriptor sets are carved out of.
///
/// Descriptor sets are not created individually - they are suballocated from a
/// pool declared up front with how many of each descriptor type it can hold.
/// Running out is a hard error rather than a resize, so the counts are sized
/// for the worst case.
class DescriptorPool {
public:
    DescriptorPool(Device& device, u32 maxSets, std::span<const vk::DescriptorPoolSize> sizes);

    DescriptorPool(const DescriptorPool&) = delete;
    DescriptorPool& operator=(const DescriptorPool&) = delete;

    vk::DescriptorSet allocate(vk::DescriptorSetLayout layout);

    /// Frees every set at once. Individual sets cannot be returned unless the
    /// pool was created with eFreeDescriptorSet, and resetting wholesale is
    /// both simpler and faster.
    void reset();

    vk::DescriptorPool handle() const { return *m_pool; }

private:
    Device& m_device;
    vk::UniqueDescriptorPool m_pool;
};

/// Writes resources into descriptor sets.
///
/// Collects the writes and submits them in one updateDescriptorSets call, since
/// each call has driver overhead and batching is free.
class DescriptorWriter {
public:
    /// Points a binding at a uniform buffer.
    DescriptorWriter& buffer(vk::DescriptorSet set, u32 binding, vk::Buffer buffer, vk::DeviceSize size,
                             vk::DescriptorType type = vk::DescriptorType::eUniformBuffer,
                             vk::DeviceSize offset = 0);

    /// Points a binding at a texture plus the sampler used to read it.
    DescriptorWriter& image(vk::DescriptorSet set, u32 binding, vk::ImageView view, vk::Sampler sampler,
                            vk::ImageLayout layout = vk::ImageLayout::eShaderReadOnlyOptimal);

    /// Points a binding at an acceleration structure.
    ///
    /// This one does not go through a VkDescriptorBufferInfo or ImageInfo at
    /// all: the handle is passed through a struct chained into the write's
    /// pNext, which is how every descriptor type added after Vulkan 1.0 is
    /// written. descriptorCount still has to be set on the write itself.
    DescriptorWriter& accelerationStructure(vk::DescriptorSet set, u32 binding,
                                            vk::AccelerationStructureKHR structure);

    void submit(vk::Device device);

private:
    // Stable storage: vk::WriteDescriptorSet holds raw pointers into these, so
    // they must not move. deque never reallocates existing elements, unlike
    // vector.
    std::deque<vk::DescriptorBufferInfo> m_bufferInfos;
    std::deque<vk::DescriptorImageInfo> m_imageInfos;
    std::deque<vk::AccelerationStructureKHR> m_structures;
    std::deque<vk::WriteDescriptorSetAccelerationStructureKHR> m_structureWrites;
    std::vector<vk::WriteDescriptorSet> m_writes;
};

} // namespace fumar::rhi
