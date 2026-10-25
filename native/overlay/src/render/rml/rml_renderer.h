// RmlUi's render interface on Vulkan.
//
// The whole UI is drawn into a layer of its own and laid over the game's frame in one pass at
// the end, so the game's swapchain format never reaches RmlUi.
#pragma once

#include <cstdint>

#include <RmlUi/Core/RenderInterface.h>
#include <vulkan/vulkan.h>

namespace rml_renderer {

// Lives for the process; draws nothing until the GPU objects exist.
Rml::RenderInterface* get();

// Device lifetime.
bool create_device_objects();
// Swapchain lifetime; only once the device is idle.
bool create_frame_objects(VkRenderPass framePass);
void destroy_frame_objects();

// Right after the slot's fence, before RmlUi updates: what it lets go of now is freed later.
void after_fence(uint32_t slot);
// Whether the frame went to the GPU; one that did not keeps nothing alive.
void frame_done(bool submitted);

// Outside any render pass. Everything RmlUi renders between the two lands in the UI layer.
void begin_frame(VkCommandBuffer cmd, VkExtent2D extent);
// False when the UI layer could not be drawn; there is then nothing to present.
bool end_frame();

// Inside the frame's render pass: the UI layer over the game's image.
void present();

}  // namespace rml_renderer
