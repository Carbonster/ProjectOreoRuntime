#include "input/active_device.h"

#include <windows.h>

namespace active_device {

namespace {

volatile LONG g_device = (LONG)Device::None;

}  // namespace

void note(Device device) { InterlockedExchange(&g_device, (LONG)device); }

Device current() { return (Device)InterlockedCompareExchange(&g_device, 0, 0); }

const char* name(Device device) {
    return device == Device::Gamepad ? "gamepad" : "mouse_keyboard";
}

}  // namespace active_device
