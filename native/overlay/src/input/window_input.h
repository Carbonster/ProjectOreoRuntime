// The game window's mouse and keyboard messages, copied for the UI.
//
// The game still receives every message; game_input keeps it from acting on them while a panel
// takes the input. The copies wait here for the render thread, which owns RmlUi.
#pragma once

#include <windows.h>

#include <vector>

namespace window_input {

struct Message {
    UINT msg;
    WPARAM wParam;
    LPARAM lParam;
    int modifiers;   // Rml::Input::KeyModifier, read when the message arrived
};

// DllMain, before any thread that could take the lock exists.
void init();

// Our window procedure in front of the game's.
void attach(HWND window);

// A fault while handling a message must not freeze the game's window thread.
void give_up();

// Render thread: everything since the last call, oldest first.
std::vector<Message> take();

}  // namespace window_input
