// Every mod's documents and values, side by side in RmlUi's one context. Render thread only.
//
// A mod is a data model of its own name: `<body data-model="fishing">` binds a document to the
// values the fishing mod sends. Mods never see each other's documents or values.
#pragma once

#include <RmlUi/Core/Context.h>

#include "ui/ui_commands.h"

namespace documents {

// `baseSheet` is linked into every document ahead of its own styles: defaults for the elements.
void attach(Rml::Context* context, const std::string& baseSheet);

void apply(const ui_commands::Command& command);

// A modal document is showing: the pointer, keys and pad belong to the UI.
bool takes_input();
int visible_count();

}  // namespace documents
