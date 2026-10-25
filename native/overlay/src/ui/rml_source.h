// A mod's RML as RmlUi gets to read it: with the Runtime's base style sheet linked ahead of the
// document's own, so the document's rules win and the defaults are there from the first layout.
#pragma once

#include <string>

namespace rml_source {

// False when `rml` has no <rml> element to put a <head> in.
bool link_base_sheet(std::string& rml, const std::string& sheetPath);

}  // namespace rml_source
