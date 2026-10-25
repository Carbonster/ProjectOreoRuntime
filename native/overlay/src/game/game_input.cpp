#include "game/game_input.h"

#include <windows.h>

#include "MinHook.h"
#include "game/game_image.h"
#include "log.h"

namespace game_input {

namespace {

// Every game question about input - "is jump pressed", "how far is the stick" - goes through
// these two cTkInputManager methods. Checked against NMS.exe of 2026-09-30 and the build before.
const char* kSigGetButtonInput =
    "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC ? 49 63 F0 41 8B D9 48 63 FA 48 8B "
    "E9 41 83 F9 FF 75 ? 8B 59 ? 83 FB 04 0F 84 ? ? ? ? 83 FB 05 0F 87";
const char* kSigGetAnalogInput =
    "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 63 FA 41 8B D8 48 8B F1 41 83 F8 FF 75 ? 8B "
    "59 ? 83 FB 04 74 ? 83 FB 05 76";

typedef bool (*PFN_GetButtonInput)(void* self, int action, int validation, int port);
typedef float (*PFN_GetAnalogInput)(void* self, int action, int port);
PFN_GetButtonInput oGetButtonInput = nullptr;
PFN_GetAnalogInput oGetAnalogInput = nullptr;

volatile LONG g_panelUp = 0;
volatile ULONGLONG g_panelUpTick = 0;
void* volatile g_inputManager = nullptr;   // as the hooks last saw it

// Stale means the overlay stopped drawing with a panel up; the game must not stay deaf.
bool panel_holds_input() {
    if (!InterlockedCompareExchange(&g_panelUp, 0, 0)) return false;
    return GetTickCount64() - g_panelUpTick < 500;
}

bool hkGetButtonInput(void* self, int action, int validation, int port) {
    if (g_inputManager != self) g_inputManager = self;
    if (panel_holds_input()) return false;
    return oGetButtonInput(self, action, validation, port);
}

float hkGetAnalogInput(void* self, int action, int port) {
    if (g_inputManager != self) g_inputManager = self;
    if (panel_holds_input()) return 0.0f;
    return oGetAnalogInput(self, action, port);
}

// The device manager lives inside the input manager; the game calls these itself when its
// window loses focus. Each only sets a flag, applied on the game's next input update.
// From NMS.exe of 2026-09-30, by disassembly.
constexpr size_t kDeviceManagerOffset = 0x10;
constexpr size_t kMouseLockedOffset = 0x2C;
enum { kLockMouse = 2, kUnlockMouse = 3, kHideMouse = 4, kShowMouse = 5, kIsMouseHidden = 6 };
typedef void (*PFN_MouseCall)(void* deviceManager);
typedef bool (*PFN_MouseQuery)(void* deviceManager);

struct CursorState {
    bool taken = false;
    bool wasLocked = false;
    bool wasHidden = false;
    bool broken = false;
};
CursorState g_cursor;   // render thread only

void follow_with_cursor(bool panelUp) {
    char* deviceManager = (char*)g_inputManager;
    if (!deviceManager) return;
    deviceManager += kDeviceManagerOffset;
    void** vtable = *(void***)deviceManager;
    if (!game_image::contains(vtable)) {
        g_cursor.broken = true;
        LOG_ERROR("input: the device manager is not where it was (vtable %p); the cursor is left "
                  "to the game.", (void*)vtable);
        return;
    }
    bool locked = *(volatile bool*)(deviceManager + kMouseLockedOffset);
    bool hidden = ((PFN_MouseQuery)vtable[kIsMouseHidden])(deviceManager);
    if (panelUp) {
        if (!g_cursor.taken) {
            g_cursor.taken = true;
            g_cursor.wasLocked = locked;
            g_cursor.wasHidden = hidden;
            LOG("input: panel up, cursor freed (it was %s, %s).", locked ? "locked" : "free",
                hidden ? "hidden" : "shown");
        }
        // Every frame: if the game takes the cursor back while the panel is up, let it go again.
        if (locked) ((PFN_MouseCall)vtable[kUnlockMouse])(deviceManager);
        if (hidden) ((PFN_MouseCall)vtable[kShowMouse])(deviceManager);
    } else if (g_cursor.taken) {
        g_cursor.taken = false;
        if (g_cursor.wasLocked) ((PFN_MouseCall)vtable[kLockMouse])(deviceManager);
        if (g_cursor.wasHidden) ((PFN_MouseCall)vtable[kHideMouse])(deviceManager);
        LOG("input: panel gone, cursor handed back.");
    }
}

int cursor_exception_filter(unsigned int code) {
    g_cursor.broken = true;
    LOG_ERROR("SEH exception while switching the game's cursor: code=0x%08X; the cursor is left "
              "to the game from now on.", code);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void install_hooks() {
    void* button = game_image::find_unique(kSigGetButtonInput, "cTkInputManager::GetButtonInput");
    void* analog = game_image::find_unique(kSigGetAnalogInput, "cTkInputManager::GetAnalogInput");
    if (!button || !analog) {
        LOG_ERROR("input: game input is left alone.");
        return;
    }
    // Both or neither: a game that still turns the camera while it ignores jump is worse.
    if (MH_CreateHook(button, (LPVOID)&hkGetButtonInput, (LPVOID*)&oGetButtonInput) != MH_OK) {
        LOG_ERROR("input: could not hook GetButtonInput.");
        return;
    }
    if (MH_CreateHook(analog, (LPVOID)&hkGetAnalogInput, (LPVOID*)&oGetAnalogInput) != MH_OK) {
        MH_RemoveHook(button);
        LOG_ERROR("input: could not hook GetAnalogInput.");
        return;
    }
    LOG("input: the game stops hearing the player while a panel with widgets is up.");
}

bool follow_panel(bool panelUp) {
    if (panelUp) g_panelUpTick = GetTickCount64();
    InterlockedExchange(&g_panelUp, panelUp ? 1 : 0);
    if (g_cursor.broken) return false;
    __try {
        follow_with_cursor(panelUp);
    } __except (cursor_exception_filter(GetExceptionCode())) {
    }
    return !g_cursor.broken && g_inputManager != nullptr;
}

}  // namespace game_input
