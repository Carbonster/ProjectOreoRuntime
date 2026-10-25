#include "ipc/outbox.h"

#include <mutex>

namespace outbox {

namespace {

// With no mod connected nothing drains it; clicks at a leftover panel must not grow it forever.
constexpr size_t kLimit = 4096;

std::mutex g_lock;
std::vector<std::string> g_events;

}  // namespace

void post(std::vector<std::string>& events) {
    if (events.empty()) return;
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_events.size() > kLimit) g_events.clear();
    for (std::string& event : events) g_events.push_back(std::move(event));
}

std::vector<std::string> take() {
    std::vector<std::string> taken;
    std::lock_guard<std::mutex> guard(g_lock);
    taken.swap(g_events);
    return taken;
}

void clear() {
    std::lock_guard<std::mutex> guard(g_lock);
    g_events.clear();
}

}  // namespace outbox
