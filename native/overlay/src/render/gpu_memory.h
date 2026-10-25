// Buffers and images with their own memory. Few and long-lived, so one allocation each.
#pragma once

#include <vulkan/vulkan.h>

namespace gpu_memory {

// Host-visible and coherent, mapped for its whole life.
struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkExtent2D extent = {0, 0};
};

struct ImageDesc {
    VkExtent2D extent;
    VkFormat format;
    VkImageUsageFlags usage;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out);
void destroy(Buffer& buffer);

// Device-local, in VK_IMAGE_LAYOUT_UNDEFINED until something moves it.
bool create_image(const ImageDesc& desc, Image& out);
void destroy(Image& image);

}  // namespace gpu_memory
