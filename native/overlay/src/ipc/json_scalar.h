// The values that cross the pipe: one JSON scalar each - true, 0.42, "text".
#pragma once

#include <string>

#include <RmlUi/Core/Variant.h>

namespace json_scalar {

// The whole of `text`, or false. Strings may hold any escape JSON allows.
bool parse(const std::string& text, Rml::Variant& out);

// Booleans, numbers and strings; anything else is written as null.
std::string write(const Rml::Variant& value);
std::string quote(const std::string& text);

}  // namespace json_scalar
