#include "ui/ui_runtime.h"

#include <RmlUi/Core/Core.h>

#include "log.h"
#include "render/rml/rml_renderer.h"
#include "ui/documents.h"
#include "ui/file_access.h"
#include "ui/polygon_element.h"
#include "ui/rml_system.h"
#include "ui/visor_filter.h"

namespace ui_runtime {

namespace {

enum class State { NotStarted, Running, Failed };

struct Runtime {
    State state = State::NotStarted;
    Rml::Context* context = nullptr;
};
Runtime g_runtime;

std::string assets_dir() {
    std::string runtime = ovlog::env("OREO_RUNTIME_DIR");
    return runtime.empty() ? std::string() : runtime + "\\assets";
}

bool start(int width, int height) {
    Rml::SetRenderInterface(rml_renderer::get());
    Rml::SetSystemInterface(rml_system::get());
    Rml::SetFileInterface(file_access::get());
    if (!Rml::Initialise()) {
        LOG_ERROR("ui: RmlUi did not start.");
        return false;
    }
    visor_filter::register_with_rmlui();
    polygon_element::register_with_rmlui();

    std::string assets = assets_dir();
    if (assets.empty()) {
        LOG_WARN("ui: OREO_RUNTIME_DIR is not set; no fonts and no base styles.");
    } else {
        file_access::allow(assets);
        // A fallback face: text in a family no document loaded is still drawn, in this one.
        if (!Rml::LoadFontFace(assets + "\\fonts\\gamefont.ttf", true))
            LOG_WARN("ui: the game font did not load.");
    }

    g_runtime.context = Rml::CreateContext("oreo", Rml::Vector2i(width, height));
    if (!g_runtime.context) {
        LOG_ERROR("ui: could not create the RmlUi context.");
        return false;
    }
    documents::attach(g_runtime.context, assets + "\\ui\\base.rcss");
    LOG("ui: RmlUi %s running at %dx%d.", Rml::GetVersion().c_str(), width, height);
    return true;
}

}  // namespace

Rml::Context* context(int width, int height) {
    if (g_runtime.state == State::NotStarted)
        g_runtime.state = start(width, height) ? State::Running : State::Failed;
    if (g_runtime.state != State::Running) return nullptr;
    if (g_runtime.context->GetDimensions() != Rml::Vector2i(width, height))
        g_runtime.context->SetDimensions(Rml::Vector2i(width, height));
    return g_runtime.context;
}

}  // namespace ui_runtime
