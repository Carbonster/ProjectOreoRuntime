// linear-, radial- and conic-gradient, plain and repeating, as RmlUi compiles them.
#pragma once

#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/RenderInterface.h>

namespace gradients {

// Zero for a shader the renderer does not know.
Rml::CompiledShaderHandle compile(const Rml::String& name, const Rml::Dictionary& parameters);
void release(Rml::CompiledShaderHandle shader);

// Binds the gradient program and its uniform block; the caller draws the geometry.
bool bind(Rml::CompiledShaderHandle shader, bool clipped);

}  // namespace gradients
