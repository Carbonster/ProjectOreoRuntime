#include "render/gpu_context.h"

#include "log.h"
#include "render/vk_functions.h"

GpuContext gpu;

namespace {

const char* vendor_name(uint32_t id) {
    switch (id) {
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "AMD";
        case 0x8086: return "Intel";
        default: return "Other";
    }
}

const char* type_name(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
        default: return "other";
    }
}

bool create_instance() {
    HMODULE loader = GetModuleHandleA("vulkan-1.dll");
    if (!loader) { LOG_ERROR("vulkan: vulkan-1.dll not loaded."); return false; }
    gpu.getProc = (PFN_vkGetInstanceProcAddr)GetProcAddress(loader, "vkGetInstanceProcAddr");
    if (!gpu.getProc) { LOG_ERROR("vulkan: no vkGetInstanceProcAddr."); return false; }

    auto createInstance = (PFN_vkCreateInstance)gpu.getProc(nullptr, "vkCreateInstance");
    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = extensions;
    if (!createInstance || createInstance(&info, nullptr, &gpu.instance) != VK_SUCCESS) {
        LOG_ERROR("vulkan: vkCreateInstance failed with the Win32 surface extensions.");
        return false;
    }
    return true;
}

// The graphics queue family that can present to `surface`, or UINT32_MAX.
uint32_t presenting_family(VkPhysicalDevice device, VkSurfaceKHR surface) {
    auto getSupport = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)gpu.getProc(
        gpu.instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    uint32_t count = 0;
    vk.GetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    VkQueueFamilyProperties families[16]{};
    if (count > 16) count = 16;
    vk.GetPhysicalDeviceQueueFamilyProperties(device, &count, families);
    for (uint32_t q = 0; q < count; ++q) {
        VkBool32 supported = VK_FALSE;
        if (getSupport(device, q, surface, &supported) == VK_SUCCESS && supported &&
            (families[q].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            return q;
    }
    return UINT32_MAX;
}

// A discrete card wins over an integrated one when both can present to the game's window.
bool select_physical_device(VkSurfaceKHR surface) {
    auto enumerate = (PFN_vkEnumeratePhysicalDevices)gpu.getProc(
        gpu.instance, "vkEnumeratePhysicalDevices");
    uint32_t count = 0;
    VkResult result = enumerate(gpu.instance, &count, nullptr);
    if (result != VK_SUCCESS || count == 0) {
        LOG_ERROR("vulkan: physical-device enumeration failed (result=%d count=%u).",
                  (int)result, count);
        return false;
    }
    VkPhysicalDevice devices[8]{};
    if (count > 8) count = 8;
    result = enumerate(gpu.instance, &count, devices);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        LOG_ERROR("vulkan: physical-device fetch failed (result=%d).", (int)result);
        return false;
    }

    bool chosenIsDiscrete = false;
    LOG("GPU inventory: enumerated=%u", count);
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties props{};
        vk.GetPhysicalDeviceProperties(devices[i], &props);
        uint32_t family = presenting_family(devices[i], surface);
        bool eligible = family != UINT32_MAX;
        // Deliberately no UUIDs, LUIDs or serials.
        LOG("GPU[%u] candidate: name=%s vendor=%s type=%s presentingFamily=%d eligible=%s "
            "vendorID=0x%04X deviceID=0x%04X vulkan=%u.%u.%u driverRaw=0x%08X",
            i, props.deviceName, vendor_name(props.vendorID), type_name(props.deviceType),
            eligible ? (int)family : -1, eligible ? "yes" : "no", props.vendorID, props.deviceID,
            VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
            VK_VERSION_PATCH(props.apiVersion), props.driverVersion);

        bool discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        if (eligible && (!gpu.physicalDevice || (discrete && !chosenIsDiscrete))) {
            gpu.physicalDevice = devices[i];
            gpu.graphicsFamily = family;
            chosenIsDiscrete = discrete;
        }
    }
    if (!gpu.physicalDevice) {
        LOG_ERROR("vulkan: no card has a graphics queue that presents to the game's window.");
        return false;
    }
    VkPhysicalDeviceProperties chosen{};
    vk.GetPhysicalDeviceProperties(gpu.physicalDevice, &chosen);
    LOG("GPU selected: name=%s type=%s graphicsPresentFamily=%u", chosen.deviceName,
        type_name(chosen.deviceType), gpu.graphicsFamily);
    return true;
}

}  // namespace

bool create_gpu_context(HWND window) {
    if (!create_instance()) return false;
    if (!vk.load(gpu.getProc, gpu.instance)) return false;

    auto createSurface = (PFN_vkCreateWin32SurfaceKHR)gpu.getProc(
        gpu.instance, "vkCreateWin32SurfaceKHR");
    auto destroySurface = (PFN_vkDestroySurfaceKHR)gpu.getProc(gpu.instance, "vkDestroySurfaceKHR");
    if (!createSurface || !destroySurface) {
        LOG_ERROR("vulkan: surface functions are unavailable.");
        return false;
    }
    VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    info.hinstance = GetModuleHandleA(nullptr);
    info.hwnd = window;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult result = createSurface(gpu.instance, &info, nullptr, &surface);
    if (result != VK_SUCCESS) {
        LOG_ERROR("vulkan: vkCreateWin32SurfaceKHR failed (result=%d).", (int)result);
        return false;
    }
    bool selected = select_physical_device(surface);
    destroySurface(gpu.instance, surface, nullptr);
    return selected;
}
