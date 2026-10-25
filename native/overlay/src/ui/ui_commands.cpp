#include "ui/ui_commands.h"

#include <mutex>

#include "log.h"

namespace ui_commands {

namespace {

// A mod sends one SET per value per frame; anything past this is a sender gone wrong.
constexpr size_t kLimit = 65536;

std::mutex g_lock;
std::vector<Command> g_commands;

}  // namespace

void post(Command command) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_commands.size() >= kLimit) {
        LOG_WARN("ui: %zu commands waiting for a frame; dropping them.", g_commands.size());
        g_commands.clear();
    }
    g_commands.push_back(std::move(command));
}

std::vector<Command> take() {
    std::vector<Command> taken;
    std::lock_guard<std::mutex> guard(g_lock);
    taken.swap(g_commands);
    return taken;
}

}  // namespace ui_commands
