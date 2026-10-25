// `filter: visor(<squeeze> <shift>)` in RCSS: the element as if on the helmet visor, bent the
// way the game bends its own HUD. Both numbers are optional; the defaults are the game's.
#pragma once

namespace visor_filter {

// Once, after Rml::Initialise.
void register_with_rmlui();

}  // namespace visor_filter
