#include "render/rml/frame_arena.h"

#include <cstring>
#include <vector>

#include "log.h"
#include "render/gpu_context.h"
#include "render/gpu_memory.h"
#include "render/vk_functions.h"

namespace frame_arena {

namespace {

constexpr uint32_t kSlots = 8;
constexpr VkDeviceSize kFirstChunk = 1 << 20;
constexpr uint32_t kMaxChunks = 64;

struct Chunk {
    gpu_memory::Buffer memory;
    VkDescriptorSet uniforms = VK_NULL_HANDLE;
    VkDeviceSize used = 0;
};

struct Arena {
    VkDescriptorSetLayout uniformLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDeviceSize uniformAlignment = 256;
    std::vector<Chunk> slots[kSlots];
    uint32_t slot = 0;
};
Arena g_arena;

VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

void destroy_chunk(Chunk& chunk) {
    if (chunk.uniforms) vk.FreeDescriptorSets(gpu.device, g_arena.pool, 1, &chunk.uniforms);
    gpu_memory::destroy(chunk.memory);
}

bool create_chunk(VkDeviceSize size, Chunk& out) {
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (!gpu_memory::create_buffer(size, usage, out.memory)) return false;
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = g_arena.pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &g_arena.uniformLayout;
    if (vk.AllocateDescriptorSets(gpu.device, &allocate, &out.uniforms) != VK_SUCCESS) {
        LOG_ERROR("arena: no descriptor set for a new chunk.");
        destroy_chunk(out);
        return false;
    }
    VkDescriptorBufferInfo range{out.memory.buffer, 0, kUniformRange};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = out.uniforms;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    write.pBufferInfo = &range;
    vk.UpdateDescriptorSets(gpu.device, 1, &write, 0, nullptr);
    return true;
}

// A chunk with `bytes` free past `alignment`, adding one twice the size of the last if needed.
Chunk* room_for(VkDeviceSize bytes, VkDeviceSize alignment) {
    std::vector<Chunk>& chunks = g_arena.slots[g_arena.slot];
    if (!chunks.empty()) {
        Chunk& last = chunks.back();
        if (align_up(last.used, alignment) + bytes <= last.memory.size) return &last;
    }
    if (chunks.size() >= kMaxChunks) return nullptr;
    VkDeviceSize size = chunks.empty() ? kFirstChunk : chunks.back().memory.size * 2;
    while (size < bytes + alignment) size *= 2;
    Chunk chunk;
    if (!create_chunk(size, chunk)) return nullptr;
    chunks.push_back(chunk);
    return &chunks.back();
}

}  // namespace

bool create(VkDescriptorSetLayout uniformLayout) {
    g_arena.uniformLayout = uniformLayout;
    VkPhysicalDeviceProperties properties{};
    vk.GetPhysicalDeviceProperties(gpu.physicalDevice, &properties);
    g_arena.uniformAlignment = properties.limits.minUniformBufferOffsetAlignment;

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kSlots * kMaxChunks};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool.maxSets = kSlots * kMaxChunks;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (vk.CreateDescriptorPool(gpu.device, &pool, nullptr, &g_arena.pool) != VK_SUCCESS) {
        LOG_ERROR("arena: could not create its descriptor pool.");
        return false;
    }
    return true;
}

void destroy() {
    for (auto& chunks : g_arena.slots) {
        for (Chunk& chunk : chunks) destroy_chunk(chunk);
        chunks.clear();
    }
    if (g_arena.pool) vk.DestroyDescriptorPool(gpu.device, g_arena.pool, nullptr);
    g_arena = Arena();
}

// Last frame's chunks become one, so a busy UI settles on a single buffer per slot.
void begin_frame(uint32_t slot) {
    g_arena.slot = slot % kSlots;
    std::vector<Chunk>& chunks = g_arena.slots[g_arena.slot];
    if (chunks.size() > 1) {
        VkDeviceSize total = 0;
        for (Chunk& chunk : chunks) {
            total += chunk.memory.size;
            destroy_chunk(chunk);
        }
        chunks.clear();
        Chunk merged;
        if (create_chunk(total, merged)) chunks.push_back(merged);
    }
    for (Chunk& chunk : chunks) chunk.used = 0;
}

ArenaSpan allocate(VkDeviceSize bytes, VkDeviceSize alignment) {
    Chunk* chunk = room_for(bytes, alignment);
    if (!chunk) return {};
    VkDeviceSize offset = align_up(chunk->used, alignment);
    chunk->used = offset + bytes;
    return {chunk->memory.buffer, offset, (char*)chunk->memory.mapped + offset, chunk->uniforms};
}

// The descriptor always covers kUniformRange bytes, so that much is kept free behind the block.
ArenaSpan push_uniform(const void* data, VkDeviceSize bytes) {
    Chunk* chunk = room_for(kUniformRange, g_arena.uniformAlignment);
    if (!chunk) return {};
    VkDeviceSize offset = align_up(chunk->used, g_arena.uniformAlignment);
    void* at = (char*)chunk->memory.mapped + offset;
    memcpy(at, data, (size_t)bytes);
    chunk->used = offset + kUniformRange;
    return {chunk->memory.buffer, offset, at, chunk->uniforms};
}

}  // namespace frame_arena
