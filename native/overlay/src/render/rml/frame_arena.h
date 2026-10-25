// Memory written once a frame and read by the GPU once: vertices, indices, uniform blocks.
//
// RmlUi keeps its geometry on the CPU side and hands it over again every frame it is drawn,
// which keeps geometry free of GPU lifetimes altogether. A UI frame is a few hundred kilobytes.
#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

struct ArenaSpan {
    VkBuffer buffer = VK_NULL_HANDLE;   // VK_NULL_HANDLE: out of memory, skip the draw
    VkDeviceSize offset = 0;
    void* data = nullptr;               // where to write the bytes
    VkDescriptorSet uniforms = VK_NULL_HANDLE;   // the buffer as a dynamic uniform block
};

namespace frame_arena {

// The largest uniform block a shader reads.
constexpr VkDeviceSize kUniformRange = 512;

// Device lifetime. `uniformLayout` has one dynamic uniform buffer at binding 0.
bool create(VkDescriptorSetLayout uniformLayout);
void destroy();

// After the slot's fence.
void begin_frame(uint32_t slot);

ArenaSpan allocate(VkDeviceSize bytes, VkDeviceSize alignment);
ArenaSpan push_uniform(const void* data, VkDeviceSize bytes);

}  // namespace frame_arena
