// Draws the overlay over the game's frame, on the GPU, between the game's render and present.
#pragma once

#include <vulkan/vulkan.h>

namespace overlay_renderer {

// Everything built for the current swapchain goes; it is rebuilt on the next present.
void reset(const char* reason, VkResult status);

// The format the game created its swapchain with. Wins over the surface guess from then on.
void use_game_format(VkFormat format);

// Returns the semaphore present must wait on instead of the game's, or VK_NULL_HANDLE when
// nothing was drawn.
VkSemaphore draw(VkSwapchainKHR swapchain, const VkPresentInfoKHR& present);

}  // namespace overlay_renderer
