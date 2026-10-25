// Hooks on the game's swapchain calls: present draws the overlay, the others say when the
// swapchain changes.
#pragma once

namespace vk_hooks {

// Finds the GPU, hooks present / acquire / create-swapchain and the game's input, and switches
// all of them on.
bool install();

}  // namespace vk_hooks
