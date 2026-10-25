// CSS filters - opacity, blur, drop-shadow, the colour matrices, mask-image - plus our own visor,
// compiled from RmlUi's names and run over the primary scratch image.
#pragma once

#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/RenderInterface.h>

namespace filters {

// Zero for a name the renderer does not know; RmlUi then draws without it.
Rml::CompiledFilterHandle compile(const Rml::String& name, const Rml::Dictionary& parameters);
// Applies the layer last saved with SaveLayerAsMaskImage.
Rml::CompiledFilterHandle mask_image();
void release(Rml::CompiledFilterHandle filter);

// In place over the primary scratch image, inside `region`.
void run(Rml::Span<const Rml::CompiledFilterHandle> filters, Rml::Rectanglei region);

}  // namespace filters
