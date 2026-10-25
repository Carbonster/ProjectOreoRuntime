// What our own command buffers cost the graphics card, from timestamps around them.
//
// One pair of timestamps per slot (swapchain image). A slot's stamps are read on its next turn,
// after its fence, so nothing ever waits on the GPU to hand a number over.
#pragma once

#include <vulkan/vulkan.h>

class GpuTimer {
public:
    // Once per device. A queue that cannot time leaves the timer off.
    void create(uint32_t slots);
    bool available() const { return pool_ != VK_NULL_HANDLE; }

    // The command buffers that wrote the stamps are gone.
    void forget();

    // After the slot's fence: the last measured cost, if there is one.
    bool collect(uint32_t slot, double& ms);

    void begin(VkCommandBuffer cmd, uint32_t slot);   // outside a render pass
    void end(VkCommandBuffer cmd, uint32_t slot);

private:
    static constexpr uint32_t kMaxSlots = 8;

    VkQueryPool pool_ = VK_NULL_HANDLE;
    bool tried_ = false;
    bool written_[kMaxSlots] = {};
    double nsPerTick_ = 0.0;
    uint64_t validMask_ = ~0ull;
};
