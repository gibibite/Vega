#include "etna/command.hpp"
#include "etna/image.hpp"

#include "utils/throw_exception.hpp"

#include <spdlog/spdlog.h>

#define COMPONENT "Etna: "

namespace {

struct EtnaCommandPool_T final {
    VkCommandPool command_pool;
    VkDevice      device;
};

struct EtnaCommandBuffer_T final {
    VkCommandBuffer command_buffer;
    VkDevice        device;
    VkCommandPool   command_pool;
};

} // namespace

namespace etna {

CommandPool::operator VkCommandPool() const noexcept
{
    return m_state ? m_state->command_pool : VkCommandPool{};
}

auto CommandPool::AllocateCommandBuffer(CommandBufferLevel level) -> UniqueCommandBuffer
{
    assert(m_state);

    auto alloc_info = VkCommandBufferAllocateInfo{

        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext              = nullptr,
        .commandPool        = m_state->command_pool,
        .level              = GetVkFlags(level),
        .commandBufferCount = 1
    };

    return CommandBuffer::Create(m_state->device, alloc_info);
}

UniqueCommandPool CommandPool::Create(VkDevice device, const VkCommandPoolCreateInfo& create_info)
{
    VkCommandPool command_pool{};

    if (auto result = vkCreateCommandPool(device, &create_info, nullptr, &command_pool); result != VK_SUCCESS) {
        throw_runtime_error(fmt::format("vkCreateCommandPool error: {}", result).c_str());
    }

    spdlog::info(COMPONENT "Created VkCommandPool {}", fmt::ptr(command_pool));

    return UniqueCommandPool(new EtnaCommandPool_T{ command_pool, device });
}

void CommandPool::Destroy() noexcept
{
    assert(m_state);

    vkDestroyCommandPool(m_state->device, m_state->command_pool, nullptr);

    spdlog::info(COMPONENT "Destroyed VkCommandPool {}", fmt::ptr(m_state->command_pool));

    delete m_state;

    m_state = nullptr;
}

CommandBuffer::operator VkCommandBuffer() const noexcept
{
    return m_state ? m_state->command_buffer : VkCommandBuffer{};
}

void CommandBuffer::Begin(CommandBufferUsageMask command_buffer_usage_mask)
{
    assert(m_state);

    auto begin_info = VkCommandBufferBeginInfo{

        .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext            = nullptr,
        .flags            = command_buffer_usage_mask.GetVkFlags(),
        .pInheritanceInfo = nullptr
    };

    if (auto result = vkBeginCommandBuffer(m_state->command_buffer, &begin_info); result != VK_SUCCESS) {
        throw_runtime_error(fmt::format("vkBeginCommandBuffer error: {}", result).c_str());
    }
}

void CommandBuffer::BeginRenderPass(Framebuffer framebuffer, SubpassContents subpass_contents)
{
    assert(m_state);

    VkOffset2D offset = { 0, 0 };
    VkExtent2D extent = framebuffer.Extent2D();

    VkClearColorValue color       = { 0, 0, 0, 0 };
    VkClearValue      clear_value = { color };

    VkRenderPassBeginInfo begin_info = {

        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext           = nullptr,
        .renderPass      = framebuffer.RenderPass(),
        .framebuffer     = framebuffer,
        .renderArea      = VkRect2D{ offset, extent },
        .clearValueCount = 1,
        .pClearValues    = &clear_value
    };

    vkCmdBeginRenderPass(m_state->command_buffer, &begin_info, GetVkFlags(subpass_contents));
}

void CommandBuffer::EndRenderPass()
{
    assert(m_state);
    vkCmdEndRenderPass(m_state->command_buffer);
}

void CommandBuffer::End()
{
    assert(m_state);
    vkEndCommandBuffer(m_state->command_buffer);
}

void CommandBuffer::BindPipeline(PipelineBindPoint pipeline_bind_point, Pipeline pipeline)
{
    assert(m_state);
    vkCmdBindPipeline(m_state->command_buffer, GetVkFlags(pipeline_bind_point), pipeline);
}

void CommandBuffer::Draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance)
{
    assert(m_state);
    vkCmdDraw(m_state->command_buffer, vertex_count, instance_count, first_vertex, first_instance);
}

void CommandBuffer::PipelineBarrier(
    Image2D               image,
    PipelineStageMask src_stage_mask,
    PipelineStageMask dst_stage_mask,
    AccessMask            src_access_mask,
    AccessMask            dst_access_mask,
    ImageLayout           old_layout,
    ImageLayout           new_layout,
    ImageAspectMask       aspect_mask)
{
    assert(m_state);

    VkImageSubresourceRange subresource_range = {

        .aspectMask     = aspect_mask.GetVkFlags(),
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1
    };

    VkImageMemoryBarrier image_memory_barrier = {

        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext               = nullptr,
        .srcAccessMask       = src_access_mask.GetVkFlags(),
        .dstAccessMask       = dst_access_mask.GetVkFlags(),
        .oldLayout           = GetVkFlags(old_layout),
        .newLayout           = GetVkFlags(new_layout),
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image,
        .subresourceRange    = subresource_range
    };

    vkCmdPipelineBarrier(
        m_state->command_buffer,
        src_stage_mask.GetVkFlags(),
        dst_stage_mask.GetVkFlags(),
        {},
        0,
        nullptr,
        0,
        nullptr,
        1,
        &image_memory_barrier);
}

void CommandBuffer::CopyImage(
    Image2D         src_image,
    ImageLayout     src_image_layout,
    Image2D         dst_image,
    ImageLayout     dst_image_layout,
    ImageAspectMask aspect_mask)
{
    assert(m_state);

    auto [width, height] = src_image.Extent();

    VkImageCopy image_copy = {

        .srcSubresource = { aspect_mask.GetVkFlags(), 0, 0, 1 },
        .srcOffset      = 0,
        .dstSubresource = { aspect_mask.GetVkFlags(), 0, 0, 1 },
        .dstOffset      = { 0, 0, 0 },
        .extent         = { width, height, 1 }
    };

    vkCmdCopyImage(
        m_state->command_buffer,
        src_image,
        GetVkFlags(src_image_layout),
        dst_image,
        GetVkFlags(dst_image_layout),
        1,
        &image_copy);
}

UniqueCommandBuffer CommandBuffer::Create(VkDevice device, const VkCommandBufferAllocateInfo& alloc_info)
{
    VkCommandBuffer command_buffer{};

    if (auto result = vkAllocateCommandBuffers(device, &alloc_info, &command_buffer); result != VK_SUCCESS) {
        throw_runtime_error(fmt::format("vkCreateCommandPool error: {}", result).c_str());
    }

    spdlog::info(COMPONENT "Allocated VkCommandBuffer {}", fmt::ptr(command_buffer));

    return UniqueCommandBuffer(new EtnaCommandBuffer_T{ command_buffer, device, alloc_info.commandPool });
}

void CommandBuffer::Destroy() noexcept
{
    assert(m_state);

    vkFreeCommandBuffers(m_state->device, m_state->command_pool, 1, &m_state->command_buffer);

    spdlog::info(COMPONENT "Destroyed VkCommandBuffer {}", fmt::ptr(m_state->command_buffer));

    delete m_state;

    m_state = nullptr;
}

} // namespace etna
