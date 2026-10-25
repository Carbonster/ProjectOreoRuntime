// Small command-buffer chores shared by the renderer.
//
// Every image the overlay owns lives in VK_IMAGE_LAYOUT_GENERAL, so moving between drawing,
// sampling and copying needs a memory barrier and never a layout change.
#pragma once

#include <functional>

#include <vulkan/vulkan.h>

namespace gpu_commands {

// Records into a command buffer of its own, submits it on the game's queue and waits. For rare
// work only: uploads and first layouts. Render thread.
bool run_now(const std::function<void(VkCommandBuffer)>& record);

// Whatever was written before is visible to whatever comes after. Coarse on purpose: the
// overlay's passes are few and small.
void barrier(VkCommandBuffer cmd);

// From UNDEFINED, once, right after the image is made.
void to_general(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect);

}  // namespace gpu_commands
