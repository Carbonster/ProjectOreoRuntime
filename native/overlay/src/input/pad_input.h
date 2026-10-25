// Pad buttons as the Runtime's controller helper reports them, by position rather than letter:
// "a" is the bottom face button on a PlayStation pad too.
#pragma once

#include <cstdint>

namespace pad_input {

enum Button : uint32_t {
    A = 1 << 0, B = 1 << 1, X = 1 << 2, Y = 1 << 3,
    Up = 1 << 4, Down = 1 << 5, Left = 1 << 6, Right = 1 << 7,
    LB = 1 << 8, RB = 1 << 9, Back = 1 << 10, Start = 1 << 11, LS = 1 << 12, RS = 1 << 13,
};

// Pipe thread. "a,up" - the buttons held right now; unknown names are ignored.
void set_held(const char* names);
uint32_t held();

}  // namespace pad_input
