// Window messages and pad buttons, handed to RmlUi on the render thread.
#pragma once

#include <RmlUi/Core/Context.h>

namespace rml_input {

// The mouse always reaches the UI, so hover works on every panel. Keys and the pad only do when
// `keys` is set - while a panel has taken the input - or the player's typing would land in it.
// The pad walks the UI the way arrows, Enter and Escape do.
void deliver(Rml::Context* context, bool keys);

}  // namespace rml_input
