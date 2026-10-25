// The only way RmlUi reads files. Documents, styles, images and fonts come from a folder a mod
// registered, or the Runtime's own assets; any other path is refused and logged.
#pragma once

#include <string>

#include <RmlUi/Core/FileInterface.h>

namespace file_access {

Rml::FileInterface* get();

// Everything below `dir` becomes readable.
void allow(const std::string& dir);

// Absolute, normalised, backslashes, no trailing one; empty if Windows cannot make sense of it.
std::string canonical(const std::string& path);
bool allowed(const std::string& path);

}  // namespace file_access
