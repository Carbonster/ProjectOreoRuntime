// RmlUi's lifetime: started on the first frame with the Runtime's interfaces, fonts and filters,
// and one context the size of the screen. Render thread only.
#pragma once

#include <RmlUi/Core/Context.h>

namespace ui_runtime {

// nullptr when RmlUi could not start; that is logged once and the UI stays off.
Rml::Context* context(int width, int height);

}  // namespace ui_runtime
