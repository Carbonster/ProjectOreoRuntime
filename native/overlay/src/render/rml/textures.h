// Textures RmlUi draws with: images, glyph atlases, saved layers. Premultiplied RGBA, each with a
// descriptor set of its own.
#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include "render/gpu_memory.h"

struct Texture {
    gpu_memory::Image image;
    VkDescriptorSet set = VK_NULL_HANDLE;
};

namespace textures {

constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

// Device lifetime. `layout` has one combined image sampler at binding 0.
bool create(VkDescriptorSetLayout layout);
void destroy();

// Images repeat, so image decorators can tile; render targets clamp at the screen's edge.
enum class Sampling { Repeat, Clamp };

VkDescriptorSet describe(VkImageView view, Sampling sampling);
void forget(VkDescriptorSet set);

// Uploaded before this returns. nullptr on failure.
Texture* from_pixels(const uint8_t* rgba, uint32_t width, uint32_t height);

// Contents left to a copy the caller records into `cmd`.
Texture* blank(VkCommandBuffer cmd, VkExtent2D size);

// Destroyed once no frame in flight can still be drawing with it.
void release(Texture* texture);

}  // namespace textures
