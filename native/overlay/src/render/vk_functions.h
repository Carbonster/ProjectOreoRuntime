// The Vulkan calls the overlay makes itself, resolved through the loader once at startup.
#pragma once

#include <vulkan/vulkan.h>

#define OREO_VULKAN_FUNCTIONS(X)                                                                 \
    X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceQueueFamilyProperties)                     \
    X(GetSwapchainImagesKHR) X(DeviceWaitIdle) X(QueueSubmit) X(QueueWaitIdle)                   \
    X(CreateImageView) X(DestroyImageView) X(CreateFramebuffer) X(DestroyFramebuffer)            \
    X(CreateRenderPass) X(DestroyRenderPass) X(CreateCommandPool) X(DestroyCommandPool)          \
    X(AllocateCommandBuffers) X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer)    \
    X(CmdBeginRenderPass) X(CmdEndRenderPass)                                                    \
    X(CreateFence) X(WaitForFences) X(ResetFences) X(CreateSemaphore)                            \
    X(CreateQueryPool) X(CmdResetQueryPool) X(CmdWriteTimestamp) X(GetQueryPoolResults)          \
    X(GetPhysicalDeviceMemoryProperties) X(AllocateMemory) X(FreeMemory)                         \
    X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory)              \
    X(CreateSampler) X(DestroySampler) X(CreateShaderModule) X(DestroyShaderModule)              \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout)                                   \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets)                   \
    X(UpdateDescriptorSets) X(CreatePipelineLayout) X(DestroyPipelineLayout)                     \
    X(CreateGraphicsPipelines) X(DestroyPipeline)                                                \
    X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants)                              \
    X(CmdSetViewport) X(CmdSetScissor) X(CmdDraw)                                                \
    X(GetPhysicalDeviceFormatProperties) X(FreeCommandBuffers) X(DestroyFence)                   \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory)          \
    X(MapMemory) X(FreeDescriptorSets) X(CmdBindVertexBuffers) X(CmdBindIndexBuffer)             \
    X(CmdDrawIndexed) X(CmdPipelineBarrier) X(CmdCopyBufferToImage) X(CmdCopyImage)              \
    X(CmdBlitImage) X(CmdResolveImage) X(CmdClearAttachments) X(CmdSetStencilReference)

struct VulkanFunctions {
#define OREO_DECLARE(name) PFN_vk##name name = nullptr;
    OREO_VULKAN_FUNCTIONS(OREO_DECLARE)
#undef OREO_DECLARE

    bool load(PFN_vkGetInstanceProcAddr getProc, VkInstance instance);
};

extern VulkanFunctions vk;
