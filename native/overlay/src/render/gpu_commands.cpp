#include "render/gpu_commands.h"

#include "log.h"
#include "render/gpu_context.h"
#include "render/vk_functions.h"

namespace gpu_commands {

namespace {

constexpr VkAccessFlags kAnyAccess =
    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;

struct OneShot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
};
OneShot g_oneShot;   // for the life of the device

bool create_one_shot() {
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = gpu.graphicsFamily;
    VkCommandBufferAllocateInfo buffer{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    buffer.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    buffer.commandBufferCount = 1;
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    bool ok = vk.CreateCommandPool(gpu.device, &pool, nullptr, &g_oneShot.pool) == VK_SUCCESS;
    buffer.commandPool = g_oneShot.pool;
    ok = ok && vk.AllocateCommandBuffers(gpu.device, &buffer, &g_oneShot.cmd) == VK_SUCCESS &&
         vk.CreateFence(gpu.device, &fence, nullptr, &g_oneShot.fence) == VK_SUCCESS;
    if (!ok) LOG_ERROR("gpu: could not create the one-shot command buffer.");
    return ok;
}

}  // namespace

bool run_now(const std::function<void(VkCommandBuffer)>& record) {
    if (!g_oneShot.fence && !create_one_shot()) return false;
    if (!gpu.queue) return false;
    VkCommandBuffer cmd = g_oneShot.cmd;
    vk.ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.BeginCommandBuffer(cmd, &begin);
    record(cmd);
    vk.EndCommandBuffer(cmd);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vk.ResetFences(gpu.device, 1, &g_oneShot.fence);
    if (vk.QueueSubmit(gpu.queue, 1, &submit, g_oneShot.fence) != VK_SUCCESS) {
        LOG_ERROR("gpu: one-shot submit failed.");
        return false;
    }
    vk.WaitForFences(gpu.device, 1, &g_oneShot.fence, VK_TRUE, UINT64_MAX);
    return true;
}

void barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memory.srcAccessMask = kAnyAccess;
    memory.dstAccessMask = kAnyAccess;
    vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          0, 1, &memory, 0, nullptr, 0, nullptr);
}

void to_general(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier change{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    change.dstAccessMask = kAnyAccess;
    change.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    change.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    change.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    change.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    change.image = image;
    change.subresourceRange = {aspect, 0, 1, 0, 1};
    vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          0, 0, nullptr, 0, nullptr, 1, &change);
}

}  // namespace gpu_commands
