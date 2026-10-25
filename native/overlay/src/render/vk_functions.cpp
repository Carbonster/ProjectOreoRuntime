#include "render/vk_functions.h"

#include "log.h"

VulkanFunctions vk;

bool VulkanFunctions::load(PFN_vkGetInstanceProcAddr getProc, VkInstance instance) {
#define OREO_LOAD(name)                                                    \
    name = (PFN_vk##name)getProc(instance, "vk" #name);                    \
    if (!name) { LOG_ERROR("vulkan: could not load vk" #name "."); return false; }
    OREO_VULKAN_FUNCTIONS(OREO_LOAD)
#undef OREO_LOAD
    return true;
}
