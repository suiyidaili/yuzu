// Copyright 2019 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <optional>
#include <span>
#include <utility>

#include "common/common_types.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class StagingBufferPool;
class VKScheduler;
class VKUpdateDescriptorQueue;

class VKComputePass {
public:
    explicit VKComputePass(const Device& device, VKDescriptorPool& descriptor_pool,
                           vk::Span<VkDescriptorSetLayoutBinding> bindings,
                           vk::Span<VkDescriptorUpdateTemplateEntryKHR> templates,
                           vk::Span<VkPushConstantRange> push_constants, std::span<const u32> code);
    ~VKComputePass();

protected:
    VkDescriptorSet CommitDescriptorSet(VKUpdateDescriptorQueue& update_descriptor_queue);

    vk::DescriptorUpdateTemplateKHR descriptor_template;
    vk::PipelineLayout layout;
    vk::Pipeline pipeline;

private:
    vk::DescriptorSetLayout descriptor_set_layout;
    std::optional<DescriptorAllocator> descriptor_allocator;
    vk::ShaderModule module;
};

class Uint8Pass final : public VKComputePass {
public:
    explicit Uint8Pass(const Device& device_, VKScheduler& scheduler_,
                       VKDescriptorPool& descriptor_pool_, StagingBufferPool& staging_buffer_pool_,
                       VKUpdateDescriptorQueue& update_descriptor_queue_);
    ~Uint8Pass();

    /// Assemble uint8 indices into an uint16 index buffer
    /// Returns a pair with the staging buffer, and the offset where the assembled data is
    std::pair<VkBuffer, VkDeviceSize> Assemble(u32 num_vertices, VkBuffer src_buffer,
                                               u32 src_offset);

private:
    VKScheduler& scheduler;
    StagingBufferPool& staging_buffer_pool;
    VKUpdateDescriptorQueue& update_descriptor_queue;
};

class QuadIndexedPass final : public VKComputePass {
public:
    explicit QuadIndexedPass(const Device& device_, VKScheduler& scheduler_,
                             VKDescriptorPool& descriptor_pool_,
                             StagingBufferPool& staging_buffer_pool_,
                             VKUpdateDescriptorQueue& update_descriptor_queue_);
    ~QuadIndexedPass();

    std::pair<VkBuffer, VkDeviceSize> Assemble(
        Tegra::Engines::Maxwell3D::Regs::IndexFormat index_format, u32 num_vertices,
        u32 base_vertex, VkBuffer src_buffer, u32 src_offset);

private:
    VKScheduler& scheduler;
    StagingBufferPool& staging_buffer_pool;
    VKUpdateDescriptorQueue& update_descriptor_queue;
};

} // namespace Vulkan
