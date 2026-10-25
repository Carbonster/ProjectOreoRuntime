#include "game/game_window.h"

#include <cstring>

#include "log.h"

namespace game_window {

namespace {

HWND g_window = nullptr;
long g_bestArea = 0;

BOOL CALLBACK consider(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || GetWindow(hwnd, GW_OWNER) || !IsWindowVisible(hwnd))
        return TRUE;

    RECT r;
    GetClientRect(hwnd, &r);
    long w = r.right - r.left, h = r.bottom - r.top;
    char cls[128] = {0};
    GetClassNameA(hwnd, cls, sizeof(cls));
    LOG_DEBUG("window candidate: hwnd=%p class=\"%s\" client=%ldx%ld", (void*)hwnd, cls, w, h);

    if (strcmp(cls, "GLFW30") == 0) {
        g_window = hwnd;
        return FALSE;
    }
    if (w > 200 && h > 200 && w * h > g_bestArea) {
        g_bestArea = w * h;
        g_window = hwnd;
    }
    return TRUE;
}

}  // namespace

HWND find() {
    if (!g_window) {
        g_bestArea = 0;
        EnumWindows(consider, 0);
    }
    return g_window;
}

void forget() { g_window = nullptr; }

bool minimized() {
    if (!g_window) return false;
    RECT rect{};
    if (!GetClientRect(g_window, &rect)) return false;
    return rect.right <= rect.left || rect.bottom <= rect.top;
}

}  // namespace game_window
