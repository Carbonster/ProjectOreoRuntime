#include "render/rml/textures.h"

#include <cstring>

#include "log.h"
#include "render/gpu_commands.h"
#include "render/gpu_context.h"
#include "render/rml/graveyard.h"
#include "render/vk_functions.h"

namespace textures {

namespace {

constexpr uint32_t kMaxSets = 1024;
constexpr VkImageUsageFlags kUsage =
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

struct Store {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkSampler samplers[2] = {};   // by Sampling
};
Store g_store;

Texture* make(VkExtent2D size) {
    auto* texture = new Texture();
    if (!gpu_memory::create_image({size, kFormat, kUsage}, texture->image)) {
        delete texture;
        return nullptr;
    }
    texture->set = describe(texture->image.view, Sampling::Repeat);
    if (!texture->set) {
        gpu_memory::destroy(texture->image);
        delete texture;
        return nullptr;
    }
    return texture;
}

bool create_sampler(VkSamplerAddressMode edges, VkSampler& out) {
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = edges;
    info.addressModeV = edges;
    info.addressModeW = edges;
    return vk.CreateSampler(gpu.device, &info, nullptr, &out) == VK_SUCCESS;
}

void destroy_now(Texture* texture) {
    forget(texture->set);
    gpu_memory::destroy(texture->image);
    delete texture;
}

}  // namespace

bool create(VkDescriptorSetLayout layout) {
    g_store.layout = layout;

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxSets};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool.maxSets = kMaxSets;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (!create_sampler(VK_SAMPLER_ADDRESS_MODE_REPEAT, g_store.samplers[(int)Sampling::Repeat]) ||
        !create_sampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, g_store.samplers[(int)Sampling::Clamp]) ||
        vk.CreateDescriptorPool(gpu.device, &pool, nullptr, &g_store.pool) != VK_SUCCESS) {
        LOG_ERROR("textures: could not create the sampler or descriptor pool.");
        return false;
    }
    return true;
}

void destroy() {
    if (g_store.pool) vk.DestroyDescriptorPool(gpu.device, g_store.pool, nullptr);
    for (VkSampler sampler : g_store.samplers)
        if (sampler) vk.DestroySampler(gpu.device, sampler, nullptr);
    g_store = Store();
}

VkDescriptorSet describe(VkImageView view, Sampling sampling) {
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = g_store.pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &g_store.layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vk.AllocateDescriptorSets(gpu.device, &allocate, &set) != VK_SUCCESS) {
        LOG_ERROR("textures: out of descriptor sets (%u).", kMaxSets);
        return VK_NULL_HANDLE;
    }
    VkDescriptorImageInfo image{g_store.samplers[(int)sampling], view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vk.UpdateDescriptorSets(gpu.device, 1, &write, 0, nullptr);
    return set;
}

void forget(VkDescriptorSet set) {
    if (set) vk.FreeDescriptorSets(gpu.device, g_store.pool, 1, &set);
}

Texture* from_pixels(const uint8_t* rgba, uint32_t width, uint32_t height) {
    VkDeviceSize bytes = (VkDeviceSize)width * height * 4;
    gpu_memory::Buffer staging;
    if (!gpu_memory::create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging)) return nullptr;
    memcpy(staging.mapped, rgba, (size_t)bytes);

    Texture* texture = make({width, height});
    bool uploaded = texture && gpu_commands::run_now([&](VkCommandBuffer cmd) {
        gpu_commands::to_general(cmd, texture->image.image, VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vk.CmdCopyBufferToImage(cmd, staging.buffer, texture->image.image, VK_IMAGE_LAYOUT_GENERAL,
                                1, &copy);
        gpu_commands::barrier(cmd);
    });
    gpu_memory::destroy(staging);
    if (texture && !uploaded) {
        destroy_now(texture);
        return nullptr;
    }
    return texture;
}

Texture* blank(VkCommandBuffer cmd, VkExtent2D size) {
    Texture* texture = make(size);
    if (texture) gpu_commands::to_general(cmd, texture->image.image, VK_IMAGE_ASPECT_COLOR_BIT);
    return texture;
}

void release(Texture* texture) {
    if (texture) graveyard::bury([texture] { destroy_now(texture); });
}

}  // namespace textures
