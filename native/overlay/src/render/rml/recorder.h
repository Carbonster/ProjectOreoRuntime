// The command buffer RmlUi's calls end up in: which target is open, and the state draws need.
//
// Render passes open lazily and close before anything outside a pass - a resolve, a blit, a
// different target - with a barrier after, so the next step sees what the last one wrote.
#pragma once

#include <cstdint>

#include <RmlUi/Core/Types.h>
#include <vulkan/vulkan.h>

#include "render/rml/pipelines.h"

namespace recorder {

// The scratch images by role. Filters swap primary and secondary instead of copying back.
enum class Scratch { Primary, Secondary, Tertiary };

void begin(VkCommandBuffer cmd, VkExtent2D extent);
void end();
VkCommandBuffer cmd();
VkExtent2D extent();

// False when the target could not be made; nothing should be drawn then.
bool open_layer(int layer);
bool open_scratch(Scratch scratch);
bool open_mask();
void close();

int scratch_index(Scratch scratch);
void swap_primary();

// An invalid region means the whole target.
void set_scissor(Rml::Rectanglei region);
void set_viewport(VkExtent2D size);
void set_stencil_reference(uint32_t reference);
// Inside an open pass, limited to `region`.
void clear_colour(Rml::Rectanglei region);
void clear_stencil(uint32_t value);

void bind(Program program, bool clipped = false);
void bind_set(uint32_t index, VkDescriptorSet set);
void bind_uniform(VkDescriptorSet set, uint32_t offset);
void push(const void* data, uint32_t bytes, uint32_t offset = 0);
void draw_fullscreen();
void draw_indexed(VkBuffer buffer, VkDeviceSize vertices, VkDeviceSize indices, uint32_t count);

// Outside any pass; they close the open one.
void resolve_layer(int layer, Scratch to, Rml::Rectanglei region);   // to the same place
void copy_to_origin(Scratch from, Rml::Rectanglei region, VkImage to);
void blit(Scratch from, Rml::Rectanglei source, Scratch to, Rml::Rectanglei destination);

}  // namespace recorder
