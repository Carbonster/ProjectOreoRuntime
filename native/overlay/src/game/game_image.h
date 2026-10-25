// NMS.exe as it sits in memory: where it starts and ends, and what is inside its code.
#pragma once

#include <cstdint>
#include <vector>

namespace game_image {

bool contains(const void* address);

// "48 8B ? 05" -> bytes, with -1 for a wildcard.
bool parse_signature(const char* text, std::vector<int>& out);

// The one place in the game's code that matches, or nullptr. Two matches are as bad as none:
// hooking the wrong one of a pair breaks something nobody would connect with us.
void* find_unique(const char* signature, const char* what);

}  // namespace game_image
