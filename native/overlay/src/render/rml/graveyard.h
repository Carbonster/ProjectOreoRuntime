// GPU objects let go of while a frame that used them may still be in flight.
//
// Frames are numbered by swapchain image. What is buried during a frame is destroyed the next
// time that image's fence is waited on: the queue finishes submits in order, so by then every
// frame that could have used it is done. A frame that submits nothing has no fence of its own,
// so what it buried waits for the next frame that does.
#pragma once

#include <cstdint>
#include <functional>

namespace graveyard {

// Right after the slot's fence: destroys what that slot's last submitted frame buried.
void begin_frame(uint32_t slot);

void bury(std::function<void()> destroy);

void end_frame(bool submitted);

// The device is idle: nothing is in flight any more.
void empty();

}  // namespace graveyard
