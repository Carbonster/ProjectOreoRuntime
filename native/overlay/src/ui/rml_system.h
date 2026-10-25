// What RmlUi asks of the platform: the time, the clipboard, and somewhere to complain.
//
// Its warnings - a typo in RCSS, a missing image - also go up the pipe, so they land in the
// mod's own log where its author is looking. Each one is said once until documents reload.
#pragma once

#include <RmlUi/Core/SystemInterface.h>

namespace rml_system {

Rml::SystemInterface* get();

// A document was loaded again: its problems are worth hearing again.
void forget_repeats();

}  // namespace rml_system
