#include "input/pad_input.h"

#include <windows.h>

#include <cstring>

#include "input/active_device.h"

namespace pad_input {

namespace {

struct Named {
    const char* name;
    Button button;
};
const Named kButtons[] = {
    {"a", A},   {"b", B},     {"x", X},       {"y", Y},         {"up", Up},     {"down", Down},
    {"left", Left}, {"right", Right}, {"lb", LB}, {"rb", RB}, {"back", Back}, {"start", Start},
    {"ls", LS}, {"rs", RS},
};

volatile LONG g_held = 0;

}  // namespace

void set_held(const char* names) {
    uint32_t mask = 0;
    const char* p = names;
    while (*p) {
        while (*p == ' ' || *p == ',') ++p;
        const char* start = p;
        while (*p && *p != ',' && *p != ' ') ++p;
        size_t length = (size_t)(p - start);
        for (const Named& entry : kButtons)
            if (length && strlen(entry.name) == length && strncmp(entry.name, start, length) == 0)
                mask |= entry.button;
    }
    // Only buttons count as using the pad: the helper does not report the sticks.
    if (mask) active_device::note(active_device::Device::Gamepad);
    InterlockedExchange(&g_held, (LONG)mask);
}

uint32_t held() { return (uint32_t)InterlockedCompareExchange(&g_held, 0, 0); }

}  // namespace pad_input
