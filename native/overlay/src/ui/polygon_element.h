// <polygon points="0,0 20,0 10,12"/>: a filled convex shape for what boxes cannot draw -
// triangles, trapezoids, arrows. Points are pixels from the element's own corner; the fill is
// its `color`, so styles, themes and transitions treat it like text.
#pragma once

namespace polygon_element {

// Once, after Rml::Initialise.
void register_with_rmlui();

}  // namespace polygon_element
