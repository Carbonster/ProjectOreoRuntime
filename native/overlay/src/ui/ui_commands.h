// What the pipe asks of the UI, handed from the pipe thread to the render thread.
//
// RmlUi belongs to the render thread alone, so the pipe never touches it: it checks a message,
// queues it here, and the next frame carries it out.
#pragma once

#include <string>
#include <vector>

#include <RmlUi/Core/Variant.h>

namespace ui_commands {

enum class Verb { Root, Load, Show, Close, Set, Disconnected };
enum class ShowMode { Hide, Show, Modal };

struct Command {
    Verb verb;
    std::string mod;
    std::string doc;    // Load, Show, Close
    std::string name;   // Set
    std::string path;   // Root, Load
    ShowMode mode = ShowMode::Show;
    Rml::Variant value;
};

void post(Command command);
std::vector<Command> take();

}  // namespace ui_commands
