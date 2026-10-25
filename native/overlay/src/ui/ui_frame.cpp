#include "ui/ui_frame.h"

#include <windows.h>

#include <RmlUi/Core/ElementDocument.h>

#include "diagnostics.h"
#include "game/game_input.h"
#include "input/rml_input.h"
#include "ui/documents.h"
#include "ui/ui_commands.h"
#include "ui/ui_runtime.h"

namespace ui_frame {

namespace {

Rml::Context* g_context = nullptr;   // this frame's; null while RmlUi is off
double g_updateMs = 0.0;

double now_ms() {
    static const double ticksPerMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return (double)f.QuadPart / 1000.0;
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / ticksPerMs;
}

bool anything_visible(Rml::Context* context) {
    for (int i = 0; i < context->GetNumDocuments(); ++i)
        if (context->GetDocument(i)->IsVisible()) return true;
    return false;
}

}  // namespace

bool update(VkExtent2D extent) {
    double start = now_ms();
    g_context = ui_runtime::context((int)extent.width, (int)extent.height);
    if (!g_context) return false;
    for (const ui_commands::Command& command : ui_commands::take()) documents::apply(command);

    bool takesInput = documents::takes_input();
    // TODO: draw a pointer of our own when the game will not show its own.
    game_input::follow_panel(takesInput);
    rml_input::deliver(g_context, takesInput);
    diagnostics::update(g_context, documents::visible_count());
    g_context->Update();
    g_updateMs = now_ms() - start;
    return anything_visible(g_context);
}

void render() {
    if (!g_context) return;
    double start = now_ms();
    g_context->Render();
    diagnostics::record_cpu_ms(g_updateMs + now_ms() - start);
}

}  // namespace ui_frame
