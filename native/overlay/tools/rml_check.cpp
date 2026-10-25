// Loads RML documents the way the overlay does, without the game, and prints what RmlUi says
// about them. With --layout, also where every element ended up; --set gives a value the
// documents bind to, as the mod would.
//
//   rml_check <assets dir> <model> <document.rml>... [--layout] [--set name=json]...
//
// Exit code: how many complaints there were.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <RmlUi/Core.h>

#include "ipc/json_scalar.h"
#include "ipc/outbox.h"
#include "no_gpu.h"
#include "ui/documents.h"
#include "ui/file_access.h"
#include "ui/polygon_element.h"
#include "ui/rml_system.h"
#include "ui/visor_filter.h"

namespace {

std::string folder_of(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

void print_layout(Rml::Element* element, int depth) {
    Rml::Vector2f at = element->GetAbsoluteOffset(Rml::BoxArea::Border);
    Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
    std::printf("%*s<%s%s%s%s%s> %.1f %.1f %.1fx%.1f%s\n", depth * 2, "",
                element->GetTagName().c_str(), element->GetId().empty() ? "" : " #",
                element->GetId().c_str(), element->GetClassNames().empty() ? "" : " .",
                element->GetClassNames().c_str(), at.x, at.y, size.x, size.y,
                element->IsVisible() ? "" : " hidden");
    for (int i = 0; i < element->GetNumChildren(); ++i) print_layout(element->GetChild(i), depth + 1);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: rml_check <assets dir> <model> <document.rml>... [--layout] "
                    "[--set name=json]...\n");
        return 2;
    }
    std::string assets = argv[1], model = argv[2];
    bool layout = false;
    std::vector<std::string> paths, values;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--layout") == 0) layout = true;
        else if (std::strcmp(argv[i], "--set") == 0 && i + 1 < argc) values.push_back(argv[++i]);
        else paths.push_back(argv[i]);
    }

    NoGpu gpu;
    Rml::SetRenderInterface(&gpu);
    Rml::SetSystemInterface(rml_system::get());
    Rml::SetFileInterface(file_access::get());
    if (!Rml::Initialise()) return 2;
    visor_filter::register_with_rmlui();
    polygon_element::register_with_rmlui();
    file_access::allow(assets);
    Rml::LoadFontFace(assets + "/fonts/gamefont.ttf", true);
    Rml::Context* context = Rml::CreateContext("check", {1920, 1080});
    documents::attach(context, assets + "/ui/base.rcss");

    for (const std::string& value : values) {
        size_t equals = value.find('=');
        ui_commands::Command set{ui_commands::Verb::Set, model, "", value.substr(0, equals)};
        if (equals == std::string::npos || !json_scalar::parse(value.substr(equals + 1), set.value)) {
            std::printf("--set %s: expected name=json\n", value.c_str());
            return 2;
        }
        documents::apply(set);
    }
    for (size_t i = 0; i < paths.size(); ++i) {
        file_access::allow(folder_of(paths[i]));
        ui_commands::Command load{ui_commands::Verb::Load, model, "doc" + std::to_string(i), "",
                                  paths[i]};
        documents::apply(load);
        ui_commands::Command show{ui_commands::Verb::Show, model, "doc" + std::to_string(i)};
        documents::apply(show);
    }
    context->Update();
    context->Render();

    int complaints = 0;
    for (const std::string& line : outbox::take()) {
        if (line.rfind("OK ", 0) == 0 || line.rfind("VAR ", 0) == 0) continue;
        std::printf("%s\n", line.c_str());
        ++complaints;
    }
    if (layout)
        for (int i = 0; i < context->GetNumDocuments(); ++i) print_layout(context->GetDocument(i), 0);
    std::printf("%d complaint(s)\n", complaints);
    Rml::Shutdown();
    return complaints;
}
