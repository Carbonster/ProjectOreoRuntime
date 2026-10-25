#include "render/gpu_timer.h"

#include "log.h"
#include "render/gpu_context.h"
#include "render/vk_functions.h"

void GpuTimer::create(uint32_t slots) {
    if (tried_) return;
    tried_ = true;
    if (slots > kMaxSlots) slots = kMaxSlots;

    VkPhysicalDeviceProperties props{};
    vk.GetPhysicalDeviceProperties(gpu.physicalDevice, &props);
    uint32_t familyCount = 0;
    vk.GetPhysicalDeviceQueueFamilyProperties(gpu.physicalDevice, &familyCount, nullptr);
    uint32_t validBits = 0;
    if (familyCount > 0 && familyCount <= 32) {
        VkQueueFamilyProperties families[32]{};
        vk.GetPhysicalDeviceQueueFamilyProperties(gpu.physicalDevice, &familyCount, families);
        if (gpu.graphicsFamily < familyCount)
            validBits = families[gpu.graphicsFamily].timestampValidBits;
    }
    if (props.limits.timestampPeriod <= 0.0f || validBits == 0) {
        LOG("GPU timing unavailable: timestampPeriod=%.3f validBits=%u.",
            props.limits.timestampPeriod, validBits);
        return;
    }

    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = slots * 2;
    if (vk.CreateQueryPool(gpu.device, &info, nullptr, &pool_) != VK_SUCCESS) {
        LOG_WARN("vkCreateQueryPool failed; no GPU cost will be reported.");
        pool_ = VK_NULL_HANDLE;
        return;
    }
    nsPerTick_ = (double)props.limits.timestampPeriod;
    validMask_ = validBits >= 64 ? ~0ull : ((1ull << validBits) - 1ull);
    forget();
    LOG("GPU timing ready: %.3f ns per tick, %u valid bits.", nsPerTick_, validBits);
}

void GpuTimer::forget() {
    for (bool& written : written_) written = false;
}

bool GpuTimer::collect(uint32_t slot, double& ms) {
    if (!available() || slot >= kMaxSlots || !written_[slot]) return false;
    uint64_t stamps[2] = {0, 0};
    VkResult result = vk.GetQueryPoolResults(gpu.device, pool_, slot * 2, 2, sizeof(stamps),
                                             stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS) return false;
    uint64_t begin = stamps[0] & validMask_;
    uint64_t end = stamps[1] & validMask_;
    if (end < begin) return false;
    ms = (double)(end - begin) * nsPerTick_ / 1000000.0;
    return true;
}

void GpuTimer::begin(VkCommandBuffer cmd, uint32_t slot) {
    if (!available() || slot >= kMaxSlots) return;
    vk.CmdResetQueryPool(cmd, pool_, slot * 2, 2);
    vk.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool_, slot * 2);
}

void GpuTimer::end(VkCommandBuffer cmd, uint32_t slot) {
    if (!available() || slot >= kMaxSlots) return;
    vk.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_, slot * 2 + 1);
    written_[slot] = true;
}
