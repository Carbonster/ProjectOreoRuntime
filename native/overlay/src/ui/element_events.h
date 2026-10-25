// What the player does to a document's elements, as EV lines for the mod that owns it.
//
// Clicks, changes and submits bubble up to the document, and are reported under the id of the
// nearest element that has one: a click on a button's label is a click on the button.
#pragma once

#include <string>

#include <RmlUi/Core/ElementDocument.h>

namespace element_events {

void listen(Rml::ElementDocument* document, const std::string& mod, const std::string& doc);

}  // namespace element_events
