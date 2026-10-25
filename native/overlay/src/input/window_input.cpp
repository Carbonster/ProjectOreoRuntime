#include "input/window_input.h"

#include <windowsx.h>

#include <climits>
#include <cstdlib>

#include <RmlUi/Core/Input.h>

#include "input/active_device.h"
#include "log.h"

namespace window_input {

namespace {

// A frame's worth of input is a few dozen messages; a stalled render thread must not grow this.
constexpr size_t kLimit = 1024;

CRITICAL_SECTION g_lock;
volatile LONG g_ready = 0;
WNDPROC g_gameWndProc = nullptr;
std::vector<Message> g_messages;

bool ready() { return InterlockedCompareExchange(&g_ready, 0, 0) != 0; }

bool wanted(UINT msg) {
    switch (msg) {
        case WM_MOUSEMOVE: case WM_MOUSELEAVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
        case WM_KILLFOCUS:
            return true;
        default:
            return false;
    }
}

// A move counts only past a few pixels: the game recentres the hidden pointer every frame
// while the mouse steers the camera, and that must not claim the mouse from a pad player.
bool is_mouse_or_keyboard_use(UINT msg, LPARAM lParam) {
    static int lastX = INT_MIN, lastY = INT_MIN;
    switch (msg) {
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            bool moved = lastX != INT_MIN && abs(x - lastX) + abs(y - lastY) >= 4;
            lastX = x;
            lastY = y;
            return moved;
        }
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_KEYDOWN: case WM_SYSKEYDOWN:
            return true;
        default:
            return false;
    }
}

int modifiers() {
    int state = 0;
    if (GetKeyState(VK_CAPITAL) & 1) state |= Rml::Input::KM_CAPSLOCK;
    if (GetKeyState(VK_NUMLOCK) & 1) state |= Rml::Input::KM_NUMLOCK;
    if (GetKeyState(VK_SHIFT) & 0x8000) state |= Rml::Input::KM_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) state |= Rml::Input::KM_CTRL;
    if (GetKeyState(VK_MENU) & 0x8000) state |= Rml::Input::KM_ALT;
    return state;
}

void record(UINT msg, WPARAM wParam, LPARAM lParam) {
    EnterCriticalSection(&g_lock);
    if (g_messages.size() >= kLimit) g_messages.erase(g_messages.begin());
    g_messages.push_back({msg, wParam, lParam, modifiers()});
    LeaveCriticalSection(&g_lock);
}

int exception_filter(unsigned int code) {
    give_up();
    LOG_ERROR("SEH exception in the input handler: code=0x%08X; the overlay stops reading the "
              "mouse and keyboard for this process.", code);
    return EXCEPTION_EXECUTE_HANDLER;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (is_mouse_or_keyboard_use(msg, lParam))
        active_device::note(active_device::Device::MouseKeyboard);
    if (ready() && wanted(msg)) {
        __try {
            record(msg, wParam, lParam);
        } __except (exception_filter(GetExceptionCode())) {
        }
    }
    if (!g_gameWndProc) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return CallWindowProcW(g_gameWndProc, hwnd, msg, wParam, lParam);
}

}  // namespace

void init() { InitializeCriticalSection(&g_lock); }

void attach(HWND window) {
    InterlockedExchange(&g_ready, 1);
    // Read first, swap second: a message can arrive the instant the swap lands.
    g_gameWndProc = (WNDPROC)GetWindowLongPtrW(window, GWLP_WNDPROC);
    if (g_gameWndProc && SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)window_proc) != 0) {
        LOG("input: mouse and keyboard reach the overlay (window %p).", (void*)window);
    } else {
        LOG_ERROR("input: could not attach to the game window (winerror=%lu); the UI will not "
                  "see the mouse or keyboard.", GetLastError());
    }
}

void give_up() {
    InterlockedExchange(&g_ready, 0);
    while (g_lock.OwningThread == (HANDLE)(ULONG_PTR)GetCurrentThreadId())
        LeaveCriticalSection(&g_lock);
}

std::vector<Message> take() {
    std::vector<Message> taken;
    EnterCriticalSection(&g_lock);
    taken.swap(g_messages);
    LeaveCriticalSection(&g_lock);
    return taken;
}

}  // namespace window_input
