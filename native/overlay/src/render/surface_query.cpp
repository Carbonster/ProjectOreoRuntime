#include "render/surface_query.h"

#include "game/game_window.h"
#include "log.h"
#include "render/gpu_context.h"

namespace {

struct SurfaceFunctions {
    PFN_vkCreateWin32SurfaceKHR create;
    PFN_vkDestroySurfaceKHR destroy;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR formats;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR capabilities;
};

bool load(SurfaceFunctions& f) {
    f.create = (PFN_vkCreateWin32SurfaceKHR)gpu.getProc(gpu.instance, "vkCreateWin32SurfaceKHR");
    f.destroy = (PFN_vkDestroySurfaceKHR)gpu.getProc(gpu.instance, "vkDestroySurfaceKHR");
    f.formats = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)gpu.getProc(
        gpu.instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    f.capabilities = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gpu.getProc(
        gpu.instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    return f.create && f.destroy && f.formats && f.capabilities;
}

// Only a guess, for when the game made its swapchain before our hooks went in. The surface's
// first format (B8G8R8A8 on the dev machine) drew our colours with red and blue swapped, so
// NMS picks R8G8B8A8_UNORM when it is offered rather than taking the first entry.
bool preferred_format(const SurfaceFunctions& f, VkSurfaceKHR surface, VkFormat& out) {
    uint32_t count = 0;
    VkResult result = f.formats(gpu.physicalDevice, surface, &count, nullptr);
    if (result != VK_SUCCESS || count == 0) {
        LOG_ERROR("surface: format query failed (result=%d count=%u).", (int)result, count);
        return false;
    }
    VkSurfaceFormatKHR formats[32]{};
    if (count > 32) count = 32;
    result = f.formats(gpu.physicalDevice, surface, &count, formats);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0) {
        LOG_ERROR("surface: format fetch failed (result=%d count=%u).", (int)result, count);
        return false;
    }
    for (uint32_t i = 0; i < count; ++i)
        LOG("  surface format[%u] = %d (colorSpace %d)", i, (int)formats[i].format,
            (int)formats[i].colorSpace);
    out = formats[0].format;
    for (uint32_t i = 0; i < count; ++i)
        if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) out = VK_FORMAT_R8G8B8A8_UNORM;
    return true;
}

}  // namespace

bool query_surface(HWND window, SurfaceInfo& out) {
    if (!window) { LOG("surface: no game window found yet."); return false; }
    SurfaceFunctions f{};
    if (!load(f)) { LOG("surface: query functions unavailable."); return false; }

    VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    info.hinstance = GetModuleHandleA(nullptr);
    info.hwnd = window;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (f.create(gpu.instance, &info, nullptr, &surface) != VK_SUCCESS) {
        LOG("surface: vkCreateWin32SurfaceKHR failed.");
        return false;
    }
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSurfaceCapabilitiesKHR caps{};
    bool haveFormat = preferred_format(f, surface, format);
    VkResult capsResult = haveFormat ? f.capabilities(gpu.physicalDevice, surface, &caps)
                                     : VK_ERROR_UNKNOWN;
    f.destroy(gpu.instance, surface, nullptr);
    if (!haveFormat) return false;
    if (capsResult != VK_SUCCESS) {
        LOG_ERROR("surface: capabilities query failed (result=%d).", (int)capsResult);
        return false;
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0 || extent.width == 0xFFFFFFFF) {
        RECT r;
        GetClientRect(window, &r);
        extent = {(uint32_t)(r.right - r.left), (uint32_t)(r.bottom - r.top)};
    }
    if (extent.width == 0 || extent.height == 0) {
        LOG_DEBUG("surface: zero extent while the game window is minimized; overlay deferred.");
        return false;
    }
    if (extent.width < 800 || extent.height < 600) {
        LOG("surface: extent %ux%u looks implausible for a game window - retrying next frame.",
            extent.width, extent.height);
        game_window::forget();
        return false;
    }

    out.format = format;
    out.extent = extent;
    out.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
    LOG("surface: format=%d extent=%ux%u minImages=%u", (int)out.format, extent.width,
        extent.height, out.minImageCount);
    return true;
}
