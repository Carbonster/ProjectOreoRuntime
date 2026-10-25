#include "render/rml/graveyard.h"

#include <vector>

namespace graveyard {

namespace {

constexpr uint32_t kSlots = 8;

using Graves = std::vector<std::function<void()>>;

Graves g_bySlot[kSlots];   // waiting for that slot's fence
Graves g_open;             // buried during the frame being built
uint32_t g_slot = 0;

void destroy_all(Graves& graves) {
    for (auto& destroy : graves) destroy();
    graves.clear();
}

}  // namespace

void begin_frame(uint32_t slot) {
    g_slot = slot % kSlots;
    destroy_all(g_bySlot[g_slot]);
}

void bury(std::function<void()> destroy) { g_open.push_back(std::move(destroy)); }

void end_frame(bool submitted) {
    if (!submitted) return;
    Graves& graves = g_bySlot[g_slot];
    for (auto& destroy : g_open) graves.push_back(std::move(destroy));
    g_open.clear();
}

void empty() {
    for (Graves& graves : g_bySlot) destroy_all(graves);
    destroy_all(g_open);
}

}  // namespace graveyard
