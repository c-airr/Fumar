#include "fumar/rhi/descriptor.hpp"

#include "fumar/core/assert.hpp"
#include "fumar/rhi/device.hpp"

namespace fumar::rhi {

DescriptorSetLayoutBuilder& DescriptorSetLayoutBuilder::binding(u32 index, vk::DescriptorType type,
                                                                vk::ShaderStageFlags stages, u32 count) {
    m_bindings.push_back(vk::DescriptorSetLayoutBinding{
        .binding = index,
        .descriptorType = type,
        .descriptorCount = count,
        .stageFlags = stages,
    });
    return *this;
}

vk::UniqueDescriptorSetLayout DescriptorSetLayoutBuilder::build(vk::Device device) const {
    return device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo{
        .bindingCount = static_cast<u32>(m_bindings.size()),
        .pBindings = m_bindings.data(),
    });
}

DescriptorPool::DescriptorPool(Device& device, u32 maxSets, std::span<const vk::DescriptorPoolSize> sizes)
    : m_device(device) {
    m_pool = device.handle().createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo{
        .maxSets = maxSets,
        .poolSizeCount = static_cast<u32>(sizes.size()),
        .pPoolSizes = sizes.data(),
    });
}

vk::DescriptorSet DescriptorPool::allocate(vk::DescriptorSetLayout layout) {
    const auto sets = m_device.handle().allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
        .descriptorPool = *m_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout,
    });
    FUMAR_VERIFY_MSG(!sets.empty(), "descriptor pool exhausted");
    return sets.front();
}

void DescriptorPool::reset() {
    m_device.handle().resetDescriptorPool(*m_pool);
}

DescriptorWriter& DescriptorWriter::buffer(vk::DescriptorSet set, u32 binding, vk::Buffer buffer,
                                           vk::DeviceSize size, vk::DescriptorType type,
                                           vk::DeviceSize offset) {
    const vk::DescriptorBufferInfo& info = m_bufferInfos.emplace_back(vk::DescriptorBufferInfo{
        .buffer = buffer,
        .offset = offset,
        .range = size,
    });

    m_writes.push_back(vk::WriteDescriptorSet{
        .dstSet = set,
        .dstBinding = binding,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = type,
        .pBufferInfo = &info,
    });
    return *this;
}

DescriptorWriter& DescriptorWriter::image(vk::DescriptorSet set, u32 binding, vk::ImageView view,
                                          vk::Sampler sampler, vk::ImageLayout layout) {
    const vk::DescriptorImageInfo& info = m_imageInfos.emplace_back(vk::DescriptorImageInfo{
        .sampler = sampler,
        .imageView = view,
        // The layout the image will be in when the shader reads it - not the
        // layout it is in right now.
        .imageLayout = layout,
    });

    m_writes.push_back(vk::WriteDescriptorSet{
        .dstSet = set,
        .dstBinding = binding,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo = &info,
    });
    return *this;
}

void DescriptorWriter::submit(vk::Device device) {
    if (m_writes.empty()) {
        return;
    }
    device.updateDescriptorSets(m_writes, {});
    m_writes.clear();
    m_bufferInfos.clear();
    m_imageInfos.clear();
}

} // namespace fumar::rhi
