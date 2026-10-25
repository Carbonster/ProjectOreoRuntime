// What the overlay costs, shown by the overlay itself in the top-right corner, as a document of
// its own.
//
// Off unless %APPDATA%\ProjectOreo\overlay.ini says [Debug] DebugHotkeys=true; re-read every
// couple of seconds, so no restart is needed.
#pragma once

#include <RmlUi/Core/Context.h>

namespace diagnostics {

void record_cpu_ms(double ms);
void record_gpu_ms(double ms);
void count_wire_bytes(long long bytes);
void count_wire_message();

// Render thread, once a frame, before the context updates.
void update(Rml::Context* context, int documentsVisible);

}  // namespace diagnostics
