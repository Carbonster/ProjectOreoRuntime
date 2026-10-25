// The pipe's messages, one line each. <mod> names a mod's data model, <doc> one of its documents.
//
// Mod -> overlay:
//   ROOT <mod> <dir>               the folder this mod's files may be read from
//   LOAD <mod> <doc> <path>        load an RML document, or load it again
//   SHOW <mod> <doc> <mode>        hide | show | modal (takes the pointer and the keys)
//   CLOSE <mod> <doc>
//   SET <mod> <name> <json>        a value documents bind to: true, 0.42, "text"
//   PAD <a,b,up,...>               pad buttons held; sent by the Runtime itself
//
// Overlay -> mod:
//   HELLO ui-format <n>            on connect
//   OK load <mod> <doc>            / ERR <verb> <detail>
//   EV <mod> <doc> <event> <id> <json>   the player clicked, changed or submitted an element
//   VAR <mod> <name> <json>        the player changed a value through a data-value binding
//   LOG <warning|error> <text>     RmlUi found something wrong with a document
//   INPUT <device>                 "gamepad" or "mouse_keyboard"; also on connect
#pragma once

#include <string>

namespace protocol {

constexpr int kUiFormat = 4;

// Pipe thread. Returns the reply to send back at once, or an empty string.
std::string handle(const char* line);

// The mod side went away: what it showed goes with it.
void disconnected();

}  // namespace protocol
