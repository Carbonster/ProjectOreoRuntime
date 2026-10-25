// What the game's swapchain looks like, read from a surface of our own on its window.
#pragma once

#include <windows.h>
#include <vulkan/vulkan.h>

struct SurfaceInfo {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {0, 0};
    uint32_t minImageCount = 2;
};

// False while the window is missing, minimized, or implausibly small.
bool query_surface(HWND window, SurfaceInfo& out);
