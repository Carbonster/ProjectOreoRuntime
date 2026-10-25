#include "render/vk_hooks.h"

#include "MinHook.h"
#include "game/game_input.h"
#include "game/game_window.h"
#include "input/window_input.h"
#include "log.h"
#include "render/gpu_context.h"
#include "render/overlay_renderer.h"

namespace vk_hooks {

namespace {

typedef VkResult(VKAPI_PTR* PFN_Present)(VkQueue, const VkPresentInfoKHR*);
typedef VkResult(VKAPI_PTR* PFN_CreateSwapchain)(VkDevice, const VkSwapchainCreateInfoKHR*,
                                                 const VkAllocationCallbacks*, VkSwapchainKHR*);
typedef VkResult(VKAPI_PTR* PFN_Acquire)(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore,
                                         VkFence, uint32_t*);
PFN_Present oPresent = nullptr;
PFN_CreateSwapchain oCreateSwapchain = nullptr;
PFN_Acquire oAcquire = nullptr;

VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
bool g_faulted = false;

bool swapchain_lost(VkResult result) {
    return result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR;
}

void follow_swapchain(VkSwapchainKHR swapchain, const char* where) {
    if (g_swapchain && g_swapchain != swapchain) overlay_renderer::reset(where, VK_SUCCESS);
    g_swapchain = swapchain;
}

VkResult present_and_track(VkQueue queue, const VkPresentInfoKHR* info) {
    VkResult result = oPresent(queue, info);
    if (swapchain_lost(result)) overlay_renderer::reset("vkQueuePresentKHR", result);
    return result;
}

VkResult present_body(VkQueue queue, const VkPresentInfoKHR* info) {
    gpu.queue = queue;
    if (!info) return present_and_track(queue, info);
    if (info->swapchainCount > 0 && info->pSwapchains)
        follow_swapchain(info->pSwapchains[0], "present observed a new swapchain");
    if (game_window::minimized()) return present_and_track(queue, info);

    VkSemaphore drawn = overlay_renderer::draw(g_swapchain, *info);
    if (drawn == VK_NULL_HANDLE) return present_and_track(queue, info);

    // Present now waits on our draw, which already consumed the game's semaphores.
    VkPresentInfoKHR ours = *info;
    ours.waitSemaphoreCount = 1;
    ours.pWaitSemaphores = &drawn;
    return present_and_track(queue, &ours);
}

int present_exception_filter(unsigned int code) {
    g_faulted = true;
    window_input::give_up();
    LOG_ERROR("SEH exception in present: code=0x%08X; overlay disabled for this process.", code);
    return EXCEPTION_EXECUTE_HANDLER;
}

VkResult VKAPI_PTR hkPresent(VkQueue queue, const VkPresentInfoKHR* info) {
    if (g_faulted) return oPresent(queue, info);
    __try {
        return present_body(queue, info);
    } __except (present_exception_filter(GetExceptionCode())) {
    }
    return oPresent(queue, info);
}

VkResult VKAPI_PTR hkAcquire(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                             VkSemaphore semaphore, VkFence fence, uint32_t* index) {
    if (!gpu.device) gpu.device = device;
    follow_swapchain(swapchain, "acquire observed a new swapchain");
    VkResult result = oAcquire(device, swapchain, timeout, semaphore, fence, index);
    if (swapchain_lost(result)) overlay_renderer::reset("vkAcquireNextImageKHR", result);
    return result;
}

VkResult VKAPI_PTR hkCreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info,
                                     const VkAllocationCallbacks* allocator,
                                     VkSwapchainKHR* swapchain) {
    VkResult result = oCreateSwapchain(device, info, allocator, swapchain);
    if (result == VK_SUCCESS && info) {
        overlay_renderer::reset("vkCreateSwapchainKHR", result);
        overlay_renderer::use_game_format(info->imageFormat);
        gpu.device = device;
        g_swapchain = *swapchain;
        LOG("game swapchain: format=%d colorSpace=%d extent=%ux%u minImages=%u",
            (int)info->imageFormat, (int)info->imageColorSpace, info->imageExtent.width,
            info->imageExtent.height, info->minImageCount);
    }
    return result;
}

struct SwapchainEntryPoints {
    void* present = nullptr;
    void* acquire = nullptr;
    void* createSwapchain = nullptr;
};

// The loader's device-level entry points are the same for every device, so a throwaway device
// is enough to find the ones the game calls.
bool find_entry_points(SwapchainEntryPoints& out) {
    auto createDevice = (PFN_vkCreateDevice)gpu.getProc(gpu.instance, "vkCreateDevice");
    auto getDeviceProc = (PFN_vkGetDeviceProcAddr)gpu.getProc(gpu.instance, "vkGetDeviceProcAddr");
    auto destroyDevice = (PFN_vkDestroyDevice)gpu.getProc(gpu.instance, "vkDestroyDevice");
    if (!createDevice || !getDeviceProc || !destroyDevice) return false;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = gpu.graphicsFamily;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = extensions;
    VkDevice device = VK_NULL_HANDLE;
    if (createDevice(gpu.physicalDevice, &info, nullptr, &device) != VK_SUCCESS) {
        LOG_ERROR("hooks: throwaway vkCreateDevice failed.");
        return false;
    }
    out.present = (void*)getDeviceProc(device, "vkQueuePresentKHR");
    out.acquire = (void*)getDeviceProc(device, "vkAcquireNextImageKHR");
    out.createSwapchain = (void*)getDeviceProc(device, "vkCreateSwapchainKHR");
    destroyDevice(device, nullptr);
    LOG_DEBUG("hooks: present=%p acquire=%p createSwapchain=%p", out.present, out.acquire,
              out.createSwapchain);
    return out.present && out.acquire;
}

bool hook(void* target, void* detour, void* original) {
    return MH_CreateHook(target, detour, (LPVOID*)original) == MH_OK;
}

}  // namespace

bool install() {
    HWND window = game_window::find();
    if (!window) {
        LOG_ERROR("hooks: no game window to choose a GPU by.");
        return false;
    }
    SwapchainEntryPoints entry;
    if (!create_gpu_context(window) || !find_entry_points(entry)) return false;

    if (MH_Initialize() != MH_OK) {
        LOG_ERROR("hooks: MH_Initialize failed.");
        return false;
    }
    bool hooked = hook(entry.present, (void*)&hkPresent, &oPresent) &&
                  hook(entry.acquire, (void*)&hkAcquire, &oAcquire) &&
                  (!entry.createSwapchain ||
                   hook(entry.createSwapchain, (void*)&hkCreateSwapchain, &oCreateSwapchain));
    if (!hooked) {
        LOG_ERROR("hooks: MH_CreateHook failed.");
        return false;
    }
    game_input::install_hooks();
    MH_EnableHook(MH_ALL_HOOKS);
    LOG("hooks: enabled (present + acquire + createSwapchain). Overlay should appear shortly.");
    return true;
}

}  // namespace vk_hooks
