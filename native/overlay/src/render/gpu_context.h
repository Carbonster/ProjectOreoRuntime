// The graphics card the game presents from, and the game's own device and queue on it.
#pragma once

#include <windows.h>
#include <vulkan/vulkan.h>

struct GpuContext {
    PFN_vkGetInstanceProcAddr getProc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;   // ours; kept alive for function loading
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t graphicsFamily = 0;

    VkDevice device = VK_NULL_HANDLE;   // the game's, seen by the swapchain hooks
    VkQueue queue = VK_NULL_HANDLE;     // the game's, seen by present
};

extern GpuContext gpu;

// Our own instance, the card that can present to `window`, and the function table.
bool create_gpu_context(HWND window);
