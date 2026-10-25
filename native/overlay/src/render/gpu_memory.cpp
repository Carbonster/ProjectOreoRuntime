#include "render/gpu_memory.h"

#include "log.h"
#include "render/gpu_context.h"
#include "render/vk_functions.h"

namespace gpu_memory {

namespace {

bool find_memory_type(uint32_t allowed, VkMemoryPropertyFlags wanted, uint32_t& out) {
    VkPhysicalDeviceMemoryProperties memory{};
    vk.GetPhysicalDeviceMemoryProperties(gpu.physicalDevice, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((allowed & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
            out = i;
            return true;
        }
    }
    return false;
}

bool allocate(const VkMemoryRequirements& needs, VkMemoryPropertyFlags wanted,
              VkDeviceMemory& out) {
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = needs.size;
    return find_memory_type(needs.memoryTypeBits, wanted, info.memoryTypeIndex) &&
           vk.AllocateMemory(gpu.device, &info, nullptr, &out) == VK_SUCCESS;
}

}  // namespace

bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkMemoryRequirements needs{};
    bool ok = vk.CreateBuffer(gpu.device, &info, nullptr, &out.buffer) == VK_SUCCESS;
    if (ok) vk.GetBufferMemoryRequirements(gpu.device, out.buffer, &needs);
    ok = ok &&
         allocate(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  out.memory) &&
         vk.BindBufferMemory(gpu.device, out.buffer, out.memory, 0) == VK_SUCCESS &&
         vk.MapMemory(gpu.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) == VK_SUCCESS;
    if (!ok) {
        LOG_ERROR("gpu: could not create a %llu-byte buffer.", (unsigned long long)size);
        destroy(out);
        return false;
    }
    out.size = size;
    return true;
}

void destroy(Buffer& buffer) {
    if (buffer.buffer) vk.DestroyBuffer(gpu.device, buffer.buffer, nullptr);
    if (buffer.memory) vk.FreeMemory(gpu.device, buffer.memory, nullptr);   // unmaps too
    buffer = Buffer();
}

bool create_image(const ImageDesc& desc, Image& out) {
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = desc.format;
    image.extent = {desc.extent.width, desc.extent.height, 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = desc.samples;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = desc.usage;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkMemoryRequirements needs{};
    bool ok = vk.CreateImage(gpu.device, &image, nullptr, &out.image) == VK_SUCCESS;
    if (ok) vk.GetImageMemoryRequirements(gpu.device, out.image, &needs);
    ok = ok && allocate(needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out.memory) &&
         vk.BindImageMemory(gpu.device, out.image, out.memory, 0) == VK_SUCCESS;

    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = out.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = desc.format;
    view.subresourceRange = {desc.aspect, 0, 1, 0, 1};
    ok = ok && vk.CreateImageView(gpu.device, &view, nullptr, &out.view) == VK_SUCCESS;
    if (!ok) {
        LOG_ERROR("gpu: could not create a %ux%u image (format %d, %d samples).", desc.extent.width,
                  desc.extent.height, (int)desc.format, (int)desc.samples);
        destroy(out);
        return false;
    }
    out.extent = desc.extent;
    return true;
}

void destroy(Image& image) {
    if (image.view) vk.DestroyImageView(gpu.device, image.view, nullptr);
    if (image.image) vk.DestroyImage(gpu.device, image.image, nullptr);
    if (image.memory) vk.FreeMemory(gpu.device, image.memory, nullptr);
    image = Image();
}

}  // namespace gpu_memory
