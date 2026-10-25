// The game's own input manager: while a panel with widgets is up, the game hears nothing and
// lets go of the pointer.
#pragma once

namespace game_input {

// After MH_Initialize; the hooks are switched on with the rest. Without both signatures the
// game's input is left alone.
void install_hooks();

// Render thread, once a frame. Returns whether the game is showing the pointer for us.
bool follow_panel(bool panelUp);

}  // namespace game_input
