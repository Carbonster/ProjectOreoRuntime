// Vulkan overlay. Draws the documents mods describe, and decides nothing.
//
// Mods speak over a named pipe (pipe_server, protocol); their RML documents live in RmlUi
// (ui/), which the overlay draws over the game's frame (render/). None of it knows what a fish
// is.

#include <windows.h>

#include "input/window_input.h"
#include "ipc/pipe_server.h"
#include "log.h"
#include "render/vk_hooks.h"

static DWORD WINAPI start_overlay(LPVOID) {
    ovlog::reset();
    LOG("=== Project Oreo Runtime overlay loaded ===");
    LOG("PID=%lu", GetCurrentProcessId());
    LOG("logging mode=%s max_file_mb=%llu keep_previous=%d",
        ovlog::threshold() == ovlog::Level::Trace ? "trace" :
        ovlog::threshold() == ovlog::Level::Debug ? "debug" : "normal",
        ovlog::max_bytes() / (1024ULL * 1024ULL), (int)ovlog::keep_previous());

    pipe_server::start();
    if (!vk_hooks::install()) LOG_ERROR("ABORT: setup failed.");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        window_input::init();
        CreateThread(nullptr, 0, start_overlay, nullptr, 0, nullptr);
    }
    return TRUE;
}
