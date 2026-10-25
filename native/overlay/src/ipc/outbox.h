// What the player did to widgets, on its way back to the mod. The render thread posts, the pipe
// thread takes: the render thread must never wait on a pipe.
#pragma once

#include <string>
#include <vector>

namespace outbox {

void post(std::vector<std::string>& events);
std::vector<std::string> take();
void clear();

}  // namespace outbox
