// One overlay frame on the CPU side: the pipe's commands carried out, input handed over, RmlUi
// updated - and later rendered into the frame's command buffer.
#pragma once

#include <vulkan/vulkan.h>

namespace ui_frame {

// After the slot's fence, before recording. False when nothing is showing: the frame then
// goes to the screen untouched.
bool update(VkExtent2D extent);

// While recording, between rml_renderer::begin_frame and end_frame.
void render();

}  // namespace ui_frame
