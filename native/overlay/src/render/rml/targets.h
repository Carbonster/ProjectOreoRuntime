// The images RmlUi draws into, all the size of the screen.
//
//   layers   multisampled colour, one per layer RmlUi has pushed; they share one stencil, which
//            holds the clip mask
//   post     single-sample scratch images that filters read from and write to
//   mask     a saved layer, kept for the mask-image filter
//
// Made on first use: a UI without filters never pays for the scratch images.
#pragma once

#include <vulkan/vulkan.h>

#include "render/gpu_memory.h"

struct DrawTarget {
    gpu_memory::Image colour;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;   // sampled; post and mask only
};

namespace targets {

constexpr int kMaxLayers = 8;
constexpr int kPostCount = 3;
constexpr int kMask = kPostCount;   // post(kMask) is the mask

// Device lifetime: formats, sample count, and the two render passes, which load and keep what
// is there and clear with vkCmdClearAttachments.
bool create_passes();
void destroy_passes();
VkRenderPass layer_pass();
VkRenderPass post_pass();
VkSampleCountFlagBits samples();

// Swapchain lifetime.
void resize(VkExtent2D extent);
void destroy();
VkExtent2D extent();

// nullptr if the image could not be made.
const DrawTarget* layer(int index);
const DrawTarget* post(int index);

}  // namespace targets
