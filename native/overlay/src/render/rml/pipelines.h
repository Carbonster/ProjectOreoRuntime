// Every pipeline the renderer draws with, built from one table.
//
// One layout serves them all: set 0 a texture, set 1 a dynamic uniform block, set 2 a second
// texture, and 128 bytes of push constants. A shader binds only what it reads.
#pragma once

#include <vulkan/vulkan.h>

enum class Program {
    // Into a layer, with or without the clip mask.
    Colour, Texture, Gradient, Composite, CompositeReplace,
    // Into the clip mask itself.
    ClipSet, ClipIntersect,
    // Between scratch images.
    Copy, CopyBlend, ColourMatrix, DropShadow, Blur, BlendMask, Visor,
    // Onto the game's frame.
    Present,
    Count
};

namespace pipelines {

constexpr uint32_t kPushBytes = 128;
constexpr uint32_t kTextureSet = 0, kUniformSet = 1, kMaskSet = 2;

// Device lifetime: layouts, and the pipelines of the layer and post passes.
bool create();
void destroy();

// Swapchain lifetime: Present depends on the game's format.
bool create_present(VkRenderPass framePass);
void destroy_present();

VkPipelineLayout layout();
VkDescriptorSetLayout texture_layout();
VkDescriptorSetLayout uniform_layout();

VkPipeline get(Program program, bool clipped = false);

}  // namespace pipelines
