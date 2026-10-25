// Whichever the player touched last: the pad, or mouse and keyboard. Mods show prompts by it.
#pragma once

namespace active_device {

enum class Device { None, MouseKeyboard, Gamepad };

void note(Device device);
Device current();
const char* name(Device device);   // as the INPUT line spells it

}  // namespace active_device
