// The window the game draws into.
#pragma once

#include <windows.h>

namespace game_window {

// The GLFW window, or the largest visible top-level window of this process. Cached.
HWND find();

// The cached window turned out to be the wrong one; look again next time.
void forget();

// A minimized window has no client area, and nothing may be drawn for it.
bool minimized();

}  // namespace game_window
