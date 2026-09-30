// Vulkan overlay. Draws the interface a mod described, and decides nothing.
//
// Everything on screen arrives over a named pipe as a document: screens, shapes, positions,
// colours, and the values that move them. This file hooks the game's swapchain and presents;
// ui_document.cpp turns the document into ImGui draw calls. Neither knows what a fish is.

#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include "MinHook.h"
#include "log.h"
#include <charconv>
#include <cmath>
#include <cstring>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <psapi.h>
#pragma comment(lib, "Psapi.lib")

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "backends/imgui_impl_win32.h"
#include "ui_document.h"
#include <climits>
#include <mutex>
#include <vector>
#include <windowsx.h>   // GET_X_LPARAM

// The header keeps this behind #if 0 so it does not drag <windows.h> in; we have it anyway.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// The interface the mod described, and the values it keeps sending. Empty until a mod hands
// one over, and an empty overlay draws nothing at all - which is the correct behaviour for a
// renderer that has been told nothing.
static oreoui::Document g_ui;

// ---- input: what the player does reaches ImGui ------------------------------------------
//
// Windows sends every mouse move, click and keystroke to the game's window procedure. Ours is
// put in front of it: each message is shown to ImGui through its own Win32 backend, then handed
// to the game unchanged. Nothing is swallowed here; the game is kept from acting on input
// further in, where it turns keys and sticks into actions - see hook_game_input.
//
// The gamepad does not come this way. The Runtime's controller helper already reads Xbox and
// PlayStation pads alike, and its buttons arrive over the pipe as PAD lines (see below), so the
// backend's own XInput reading is compiled out.
//
// Three threads feed ImGui's input queue - the game's window thread, the pipe thread with PAD,
// and the render thread when it starts a frame - and that queue is not thread-safe. One lock
// covers all three. A critical section because it is re-entrant: the backend calls SetCapture,
// which can send the window a message on the same thread while the lock is already held.
static CRITICAL_SECTION g_inputLock;
static WNDPROC g_gameWndProc = nullptr;
static volatile LONG g_inputReady = 0;   // ImGui context and Win32 backend both up

// A fault while the input lock is held must not leave it held: the next message the game's
// window gets would wait on it forever, and a frozen game is worse than one without an
// overlay. Runs on the faulting thread, so "owned by this thread" is the lock to give back.
static void give_up_input() {
    InterlockedExchange(&g_inputReady, 0);
    while (g_inputLock.OwningThread == (HANDLE)(ULONG_PTR)GetCurrentThreadId())
        LeaveCriticalSection(&g_inputLock);
}

static int input_exception_filter(unsigned int code) {
    give_up_input();
    LOG_ERROR("SEH exception in the input handler: code=0x%08X; the overlay stops reading the "
              "mouse and keyboard for this process.", code);
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---- which device the player is using ----------------------------------------------------
//
// Told to the mod as INPUT lines, so a panel can show the prompts for whatever is in the
// player's hands - "press A" or "click". The device touched last wins. Written by the game's
// window thread and by the pipe thread with PAD; the pipe thread sends it on a change and again
// on every connect.
enum { kDeviceNone = 0, kDeviceMouseKeyboard = 1, kDeviceGamepad = 2 };
static volatile LONG g_inputDevice = kDeviceNone;

// Whether a window message means the player is using the mouse or the keyboard. A move counts
// only once it covers a few pixels: while the mouse steers the camera the game keeps putting
// the hidden pointer back in the middle of the window, and a pointer set back to where it
// already was must not claim the mouse while the player is holding a pad.
static bool is_mouse_or_keyboard_use(UINT msg, LPARAM lParam) {
    static int lastX = INT_MIN, lastY = INT_MIN;   // window thread only
    switch (msg) {
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            bool moved = lastX != INT_MIN && abs(x - lastX) + abs(y - lastY) >= 4;
            lastX = x;
            lastY = y;
            return moved;
        }
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            return true;
        default:
            return false;
    }
}

static LRESULT CALLBACK hkWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (is_mouse_or_keyboard_use(msg, lParam))
        InterlockedExchange(&g_inputDevice, kDeviceMouseKeyboard);
    if (InterlockedCompareExchange(&g_inputReady, 0, 0)) {
        __try {
            EnterCriticalSection(&g_inputLock);
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
            LeaveCriticalSection(&g_inputLock);
        } __except (input_exception_filter(GetExceptionCode())) {
        }
    }
    // Always, whatever ImGui made of it: the game keeps every message it would have had.
    if (!g_gameWndProc) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return CallWindowProcW(g_gameWndProc, hwnd, msg, wParam, lParam);
}

// Gamepad buttons as the controller helper names them, and what ImGui calls each. A, B, X and
// Y are positions, not letters: "a" is the bottom face button on a PlayStation pad too.
struct PadKey {
    const char* name;
    ImGuiKey key;
};
static const PadKey kPadKeys[] = {
    {"a", ImGuiKey_GamepadFaceDown},   {"b", ImGuiKey_GamepadFaceRight},
    {"x", ImGuiKey_GamepadFaceLeft},   {"y", ImGuiKey_GamepadFaceUp},
    {"up", ImGuiKey_GamepadDpadUp},    {"down", ImGuiKey_GamepadDpadDown},
    {"left", ImGuiKey_GamepadDpadLeft}, {"right", ImGuiKey_GamepadDpadRight},
    {"lb", ImGuiKey_GamepadL1},        {"rb", ImGuiKey_GamepadR1},
    {"back", ImGuiKey_GamepadBack},    {"start", ImGuiKey_GamepadStart},
    {"ls", ImGuiKey_GamepadL3},        {"rs", ImGuiKey_GamepadR3},
};
static const int kPadKeyCount = (int)(sizeof(kPadKeys) / sizeof(kPadKeys[0]));
static bool g_padDown[kPadKeyCount] = {};

// "a,up" -> which of kPadKeys are held. Only changes are passed to ImGui, the way a real
// backend reports a pad.
static void apply_pad(const char* list) {
    bool down[kPadKeyCount] = {};
    const char* p = list;
    while (*p) {
        while (*p == ' ' || *p == ',') ++p;
        const char* start = p;
        while (*p && *p != ',' && *p != ' ') ++p;
        size_t length = (size_t)(p - start);
        for (int i = 0; i < kPadKeyCount && length > 0; ++i)
            if (strlen(kPadKeys[i].name) == length && strncmp(kPadKeys[i].name, start, length) == 0)
                down[i] = true;
    }
    if (!InterlockedCompareExchange(&g_inputReady, 0, 0)) return;
    EnterCriticalSection(&g_inputLock);
    ImGuiIO& io = ImGui::GetIO();
    for (int i = 0; i < kPadKeyCount; ++i) {
        if (down[i] == g_padDown[i]) continue;
        io.AddKeyEvent(kPadKeys[i].key, down[i]);
        g_padDown[i] = down[i];
    }
    LeaveCriticalSection(&g_inputLock);
}

// What the player did to a widget, on its way back to the mod. Filled by the render thread,
// written out by the pipe thread: the render thread must never wait on a pipe.
static std::mutex g_outboxLock;
static std::vector<std::string> g_outbox;

// ---- input: the game goes deaf while a panel with widgets is up --------------------------
//
// The game never looks at keys, mouse buttons or sticks directly. Its code asks one object,
// cTkInputManager, about actions - "is jump pressed", "how far is the look stick pushed" - and
// that object answers from whatever device the player uses. Every question goes through two of
// its methods: GetButtonInput for yes/no, GetAnalogInput for how much. While a panel with
// widgets is on screen both answer "no" and "0", and the game behaves as if nobody touched
// anything. The overlay still sees everything: the mouse and keyboard reach ImGui through the
// window procedure above, the pad through the controller helper.
//
// Found by byte signature rather than address, because the game updates often and addresses
// move with every build. Checked against NMS.exe of 2026-09-30 and the build before it: both
// patterns hit exactly once. If either is missing the overlay works as before, game input
// untouched.
static const char* kSigGetButtonInput =
    "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC ? 49 63 F0 41 8B D9 48 63 FA 48 8B "
    "E9 41 83 F9 FF 75 ? 8B 59 ? 83 FB 04 0F 84 ? ? ? ? 83 FB 05 0F 87";
static const char* kSigGetAnalogInput =
    "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 63 FA 41 8B D8 48 8B F1 41 83 F8 FF 75 ? 8B "
    "59 ? 83 FB 04 74 ? 83 FB 05 76";

typedef bool  (*PFN_GetButtonInput)(void* self, int action, int validation, int port);
typedef float (*PFN_GetAnalogInput)(void* self, int action, int port);
static PFN_GetButtonInput oGetButtonInput = nullptr;
static PFN_GetAnalogInput oGetAnalogInput = nullptr;

// Set by the render thread each frame, read by whichever game thread asks about input.
static volatile LONG      g_panelHasInput = 0;
static volatile ULONGLONG g_panelHasInputTick = 0;
// The game's input manager, as the hooks last saw it. The cursor is switched through it.
static void* volatile     g_gameInput = nullptr;
static uintptr_t          g_gameImageStart = 0, g_gameImageEnd = 0;

static bool panel_holds_input() {
    if (!InterlockedCompareExchange(&g_panelHasInput, 0, 0)) return false;
    // Stale means the overlay stopped drawing with a panel up - a fault, a swapchain that never
    // came back. The game must not stay deaf because the renderer went away.
    return GetTickCount64() - g_panelHasInputTick < 500;
}

static bool hkGetButtonInput(void* self, int action, int validation, int port) {
    if (g_gameInput != self) g_gameInput = self;
    if (panel_holds_input()) return false;
    return oGetButtonInput(self, action, validation, port);
}

static float hkGetAnalogInput(void* self, int action, int port) {
    if (g_gameInput != self) g_gameInput = self;
    if (panel_holds_input()) return 0.0f;
    return oGetAnalogInput(self, action, port);
}

// "48 8B ? 05" -> bytes, with -1 for a wildcard.
static bool parse_signature(const char* text, std::vector<int>& out) {
    out.clear();
    for (const char* p = text; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { out.push_back(-1); while (*p == '?') ++p; continue; }
        int value = 0;
        auto parsed = std::from_chars(p, p + 2, value, 16);
        if (parsed.ec != std::errc() || parsed.ptr != p + 2) return false;
        out.push_back(value);
        p += 2;
    }
    return !out.empty();
}

// The one place in the game's code that matches, or nullptr. Two matches is as bad as none:
// hooking the wrong one of a pair breaks something nobody would think to connect with us.
static void* find_in_game_code(const char* signature, const char* what) {
    std::vector<int> pattern;
    if (!parse_signature(signature, pattern)) {
        LOG_ERROR("input: signature for %s does not parse.", what);
        return nullptr;
    }
    auto* image = (uint8_t*)GetModuleHandleW(nullptr);
    auto* nt = (IMAGE_NT_HEADERS*)(image + ((IMAGE_DOS_HEADER*)image)->e_lfanew);
    g_gameImageStart = (uintptr_t)image;
    g_gameImageEnd = (uintptr_t)image + nt->OptionalHeader.SizeOfImage;

    uint8_t* found = nullptr;
    int count = 0;
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* start = image + section->VirtualAddress;
        size_t size = section->Misc.VirtualSize;
        if (size < pattern.size()) continue;
        for (size_t i = 0; i + pattern.size() <= size; ++i) {
            size_t k = 0;
            while (k < pattern.size() && (pattern[k] < 0 || start[i + k] == (uint8_t)pattern[k])) ++k;
            if (k == pattern.size()) { found = start + i; ++count; }
        }
    }
    if (count != 1) {
        LOG_ERROR("input: %s matched %d times in the game's code, expected once. Game input is "
                  "left alone.", what, count);
        return nullptr;
    }
    LOG("input: %s at NMS.exe+0x%llX.", what, (unsigned long long)(found - image));
    return found;
}

// Called once MinHook is up; the hooks are switched on with everything else.
static void hook_game_input() {
    void* button = find_in_game_code(kSigGetButtonInput, "cTkInputManager::GetButtonInput");
    void* analog = find_in_game_code(kSigGetAnalogInput, "cTkInputManager::GetAnalogInput");
    if (!button || !analog) return;
    // Both or neither: a game that still moves the camera while it ignores jump is worse than
    // one that ignores the panel entirely.
    if (MH_CreateHook(button, (LPVOID)&hkGetButtonInput, reinterpret_cast<LPVOID*>(&oGetButtonInput)) != MH_OK)
        { LOG_ERROR("input: could not hook GetButtonInput."); return; }
    if (MH_CreateHook(analog, (LPVOID)&hkGetAnalogInput, reinterpret_cast<LPVOID*>(&oGetAnalogInput)) != MH_OK) {
        MH_RemoveHook(button);
        LOG_ERROR("input: could not hook GetAnalogInput.");
        return;
    }
    LOG("input: the game stops hearing the player while a panel with widgets is up.");
}

// ---- cursor: the game lets go of the mouse while a panel with widgets is up ---------------
//
// Walking around, the game hides the pointer and holds it in the middle of the window, so the
// mouse turns the camera. A panel of buttons is useless like that. The game has its own calls
// for letting go - it makes them when its window loses focus - and they live on its device
// manager, which sits inside the input manager the hooks above are handed.
//
// From NMS.exe of 2026-09-30, by disassembly: the input manager's constructor writes the device
// manager's vtable at +0x10. Each call only sets a flag; the game applies it on its next input
// update.
static const size_t kDeviceManagerOffset = 0x10;
static const size_t kMouseLockedOffset = 0x2C;   // the byte LockMouse and UnlockMouse set
enum { kLockMouse = 2, kUnlockMouse = 3, kHideMouse = 4, kShowMouse = 5, kIsMouseHidden = 6 };
typedef void (*PFN_MouseCall)(void* deviceManager);
typedef bool (*PFN_MouseQuery)(void* deviceManager);

static bool g_cursorTaken = false;        // render thread only
static bool g_cursorWasLocked = false;
static bool g_cursorWasHidden = false;
static bool g_cursorBroken = false;

static void follow_panel_with_cursor(bool panelUp) {
    char* deviceManager = (char*)g_gameInput;
    if (!deviceManager) return;
    deviceManager += kDeviceManagerOffset;
    void** vtable = *(void***)deviceManager;
    if ((uintptr_t)vtable < g_gameImageStart || (uintptr_t)vtable >= g_gameImageEnd) {
        g_cursorBroken = true;
        LOG_ERROR("input: the device manager is not where it was (vtable %p); the cursor is left "
                  "to the game.", (void*)vtable);
        return;
    }
    bool locked = *(volatile bool*)(deviceManager + kMouseLockedOffset);
    bool hidden = ((PFN_MouseQuery)vtable[kIsMouseHidden])(deviceManager);
    if (panelUp) {
        if (!g_cursorTaken) {
            g_cursorTaken = true;
            g_cursorWasLocked = locked;
            g_cursorWasHidden = hidden;
            LOG("input: panel up, cursor freed (it was %s, %s).", locked ? "locked" : "free",
                hidden ? "hidden" : "shown");
        }
        // Every frame, not once: if the game takes the cursor back while the panel is up, it
        // gets let go again.
        if (locked) ((PFN_MouseCall)vtable[kUnlockMouse])(deviceManager);
        if (hidden) ((PFN_MouseCall)vtable[kShowMouse])(deviceManager);
    } else if (g_cursorTaken) {
        g_cursorTaken = false;
        if (g_cursorWasLocked) ((PFN_MouseCall)vtable[kLockMouse])(deviceManager);
        if (g_cursorWasHidden) ((PFN_MouseCall)vtable[kHideMouse])(deviceManager);
        LOG("input: panel gone, cursor handed back.");
    }
}

static int cursor_exception_filter(unsigned int code) {
    g_cursorBroken = true;
    LOG_ERROR("SEH exception while switching the game's cursor: code=0x%08X; the cursor is left "
              "to the game from now on.", code);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Render thread, once a frame. Returns whether the game is showing the pointer for us.
static bool follow_panel(bool panelUp) {
    if (panelUp) g_panelHasInputTick = GetTickCount64();
    InterlockedExchange(&g_panelHasInput, panelUp ? 1 : 0);
    if (g_cursorBroken) return false;
    __try {
        follow_panel_with_cursor(panelUp);
    } __except (cursor_exception_filter(GetExceptionCode())) {
    }
    return !g_cursorBroken && g_gameInput != nullptr;
}

// ---- imgui allocator tracking (overlay-specific memory footprint) -------------------
static size_t g_imguiLiveBytes = 0;
static size_t g_imguiLiveAllocs = 0;
static void* ImGuiAllocFn(size_t sz, void*) {
    size_t* p = (size_t*)malloc(sz + sizeof(size_t));
    if (!p) return nullptr;
    *p = sz; g_imguiLiveBytes += sz; g_imguiLiveAllocs++;
    return p + 1;
}
static void ImGuiFreeFn(void* ptr, void*) {
    if (!ptr) return;
    size_t* p = (size_t*)ptr - 1;
    g_imguiLiveBytes -= *p; g_imguiLiveAllocs--;
    free(p);
}

// The overlay used to keep the fishing panel's position, theme and hotkeys here, in
// %APPDATA%\FishingMod\mod_settings.ini. All three were decisions about how one mod's
// interface looked, made by the renderer - which is exactly what moving the UI into the
// mod's own file was for. Where a panel sits is now a line in that file, and the mod moves
// it at run time with a POS message.
//
// Logging settings used to come from a hardcoded fishing_mod.ini too. They arrive in the
// environment now; see log.h.

// ---- captured / created state -------------------------------------------------------
static PFN_vkGetInstanceProcAddr g_gipa = nullptr;
static VkInstance        g_dummyInstance = VK_NULL_HANDLE;   // kept alive: physdev + fn loading
static VkPhysicalDevice  g_physicalDevice = VK_NULL_HANDLE;
static uint32_t          g_gfxFamily = 0;

static VkDevice          g_device = VK_NULL_HANDLE;          // game's device (from swapchain hook)
static VkQueue           g_queue = VK_NULL_HANDLE;           // game's present/graphics queue (present hook)
static VkSwapchainKHR    g_swapchain = VK_NULL_HANDLE;
static VkFormat          g_format = VK_FORMAT_UNDEFINED;
static VkExtent2D        g_extent = {0, 0};
static uint32_t          g_minImageCount = 2;

static bool              g_resourcesReady = false;
static bool              g_imguiInited = false;

// Vulkan funcs we call directly (resolved via loader trampolines).
static PFN_vkGetSwapchainImagesKHR pvkGetSwapchainImagesKHR = nullptr;
static PFN_vkCreateImageView       pvkCreateImageView = nullptr;
static PFN_vkCreateRenderPass      pvkCreateRenderPass = nullptr;
static PFN_vkCreateFramebuffer     pvkCreateFramebuffer = nullptr;
static PFN_vkCreateCommandPool     pvkCreateCommandPool = nullptr;
static PFN_vkAllocateCommandBuffers pvkAllocateCommandBuffers = nullptr;
static PFN_vkBeginCommandBuffer    pvkBeginCommandBuffer = nullptr;
static PFN_vkCmdBeginRenderPass    pvkCmdBeginRenderPass = nullptr;
static PFN_vkCmdEndRenderPass      pvkCmdEndRenderPass = nullptr;
static PFN_vkEndCommandBuffer      pvkEndCommandBuffer = nullptr;
static PFN_vkQueueSubmit           pvkQueueSubmit = nullptr;
static PFN_vkQueueWaitIdle         pvkQueueWaitIdle = nullptr;   // only used in the rare no-semaphore fallback
static PFN_vkResetCommandBuffer    pvkResetCommandBuffer = nullptr;
static PFN_vkCreateFence           pvkCreateFence = nullptr;
static PFN_vkWaitForFences         pvkWaitForFences = nullptr;
static PFN_vkResetFences           pvkResetFences = nullptr;
static PFN_vkCreateSemaphore       pvkCreateSemaphore = nullptr;
static PFN_vkDeviceWaitIdle        pvkDeviceWaitIdle = nullptr;
static PFN_vkDestroyFramebuffer    pvkDestroyFramebuffer = nullptr;
static PFN_vkDestroyImageView      pvkDestroyImageView = nullptr;
static PFN_vkDestroyRenderPass     pvkDestroyRenderPass = nullptr;
static PFN_vkDestroyCommandPool    pvkDestroyCommandPool = nullptr;
// GPU timing. Timestamps around our own command buffer are the only honest way to say what
// the overlay costs the graphics card; everything else is guesswork from the overall frame
// rate. All core Vulkan 1.0 - no extension, no vendor SDK, no external profiler.
static PFN_vkCreateQueryPool       pvkCreateQueryPool = nullptr;
static PFN_vkCmdResetQueryPool     pvkCmdResetQueryPool = nullptr;
static PFN_vkCmdWriteTimestamp     pvkCmdWriteTimestamp = nullptr;
static PFN_vkGetQueryPoolResults   pvkGetQueryPoolResults = nullptr;
static PFN_vkGetPhysicalDeviceProperties pvkGetPhysicalDeviceProperties = nullptr;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties pvkGetPhysicalDeviceQueueFamilyProperties = nullptr;

// Per-swapchain-image resources.
static const uint32_t MAX_IMAGES = 8;
static uint32_t        g_imageCount = 0;
static VkImageView     g_views[MAX_IMAGES] = {};
static VkFramebuffer   g_framebuffers[MAX_IMAGES] = {};
static VkCommandBuffer g_cmds[MAX_IMAGES] = {};
static VkRenderPass    g_renderPass = VK_NULL_HANDLE;
static VkCommandPool   g_cmdPool = VK_NULL_HANDLE;

// Per-image sync: g_fences[i] tracks completion of our overlay submit that used image i's
// command buffer (so we don't reset a command buffer still in flight); g_renderSem[i] is signaled
// by that submit and waited on by present. A fixed pool of MAX_IMAGES is created ONCE and reused
// across swapchain recreations - format/extent independent, so no per-rebuild churn or leak.
static VkFence         g_fences[MAX_IMAGES] = {};
static VkSemaphore     g_renderSem[MAX_IMAGES] = {};
static bool            g_syncCreated = false;

// Two timestamps per swapchain image: one before our render pass, one after. Read back on the
// next turn of that image, once its fence says the submit finished - so nothing ever waits on
// the GPU to hand a number over.
static VkQueryPool     g_queryPool = VK_NULL_HANDLE;
static bool            g_gpuTimingReady = false;
static bool            g_gpuTimingWritten[MAX_IMAGES] = {};
static double          g_timestampPeriod = 0.0;    // nanoseconds per tick, from the device
static uint64_t        g_timestampMask = ~0ull;    // some queues report fewer than 64 valid bits

// ---- present / createswapchain / acquire hooks --------------------------------------
typedef VkResult(VKAPI_PTR* PFN_Present)(VkQueue, const VkPresentInfoKHR*);
typedef VkResult(VKAPI_PTR* PFN_CreateSwapchain)(VkDevice, const VkSwapchainCreateInfoKHR*,
                                                 const VkAllocationCallbacks*, VkSwapchainKHR*);
typedef VkResult(VKAPI_PTR* PFN_Acquire)(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
static PFN_Present         oPresent = nullptr;
static PFN_CreateSwapchain oCreateSwapchain = nullptr;
static PFN_Acquire         oAcquire = nullptr;
static bool                g_presentFaulted = false;

static void destroy_swapchain_resources(const char* reason, VkResult status);

// Find the game's GLFW window, with the largest process window as fallback.
static HWND g_hwnd = nullptr;
static long g_bestWndArea = 0;
static BOOL CALLBACK enum_wnd(HWND hwnd, LPARAM) {
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && GetWindow(hwnd, GW_OWNER) == nullptr && IsWindowVisible(hwnd)) {
        RECT r; GetClientRect(hwnd, &r);
        long w = r.right - r.left, h = r.bottom - r.top;
        char cls[128] = {0}; GetClassNameA(hwnd, cls, sizeof(cls));
        LOG_DEBUG("enum_wnd candidate: hwnd=%p class=\"%s\" client=%ldx%ld", (void*)hwnd, cls, w, h);
        // Prefer the GLFW window and fall back if its class changes.
        if (strcmp(cls, "GLFW30") == 0) {
            g_hwnd = hwnd;
            g_bestWndArea = 0x7FFFFFFF;   // lock it in; heuristic below can't override
            return FALSE;                 // found the real game window, stop enumerating
        }
        if (w > 200 && h > 200) {
            long area = w * h;
            if (area > g_bestWndArea) { g_bestWndArea = area; g_hwnd = hwnd; }
        }
    }
    return TRUE;  // keep going - want GLFW30, else the largest match
}

// Derive swapchain format+extent by creating our own surface on the game window and
// querying it - avoids needing to catch swapchain (re)creation.
static bool derive_format_extent() {
    if (!g_hwnd) { EnumWindows(enum_wnd, 0); }
    if (!g_hwnd) { LOG("derive: no game HWND found yet."); return false; }

    auto pCreateWin32Surface = (PFN_vkCreateWin32SurfaceKHR)g_gipa(g_dummyInstance, "vkCreateWin32SurfaceKHR");
    auto pGetSurfFormats = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)g_gipa(g_dummyInstance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    auto pGetSurfCaps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)g_gipa(g_dummyInstance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    auto pDestroySurface = (PFN_vkDestroySurfaceKHR)g_gipa(g_dummyInstance, "vkDestroySurfaceKHR");
    if (!pCreateWin32Surface || !pGetSurfFormats || !pGetSurfCaps || !pDestroySurface) {
        LOG("derive: surface query functions unavailable."); return false;
    }

    VkWin32SurfaceCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.hinstance = GetModuleHandleA(nullptr);
    sci.hwnd = g_hwnd;
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    if (pCreateWin32Surface(g_dummyInstance, &sci, nullptr, &surf) != VK_SUCCESS) {
        LOG("derive: vkCreateWin32SurfaceKHR failed."); return false;
    }

    uint32_t fcount = 0;
    VkResult formatResult = pGetSurfFormats(g_physicalDevice, surf, &fcount, nullptr);
    if (formatResult != VK_SUCCESS || fcount == 0) {
        LOG_ERROR("derive: surface format query failed or returned no formats (result=%d fcount=%u).",
                  (int)formatResult, fcount);
        pDestroySurface(g_dummyInstance, surf, nullptr);
        return false;
    }
    VkSurfaceFormatKHR formats[32]{}; if (fcount > 32) fcount = 32;
    formatResult = pGetSurfFormats(g_physicalDevice, surf, &fcount, formats);
    if ((formatResult != VK_SUCCESS && formatResult != VK_INCOMPLETE) || fcount == 0) {
        LOG_ERROR("derive: surface format fetch failed or returned no formats (result=%d fcount=%u).",
                  (int)formatResult, fcount);
        pDestroySurface(g_dummyInstance, surf, nullptr);
        return false;
    }
    for (uint32_t i = 0; i < fcount; ++i)
        LOG("  surface format[%u] = %d (colorSpace %d)", i, (int)formats[i].format, (int)formats[i].colorSpace);
    // Use the surface's preferred (first) format - that's almost always what the engine
    // created its swapchain with, so our render pass / views match and colors are correct.
    VkFormat chosen = formats[0].format;
    VkSurfaceCapabilitiesKHR caps{};
    VkResult capsResult = pGetSurfCaps(g_physicalDevice, surf, &caps);

    pDestroySurface(g_dummyInstance, surf, nullptr);
    if (capsResult != VK_SUCCESS) {
        LOG_ERROR("derive: surface capabilities query failed (result=%d).", (int)capsResult);
        return false;
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0 || extent.width == 0xFFFFFFFF) {
        RECT r; GetClientRect(g_hwnd, &r);
        extent.width = r.right - r.left; extent.height = r.bottom - r.top;
    }
    if (extent.width == 0 || extent.height == 0) {
        LOG_DEBUG("derive: zero extent while the game window is minimized; overlay deferred.");
        return false;
    }

    g_format = chosen;
    g_extent = extent;
    g_minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
    LOG("derived (via surface query): format=%d extent=%ux%u minImages=%u fcount=%u",
        (int)g_format, g_extent.width, g_extent.height, g_minImageCount, fcount);

    // Reject implausible dimensions and retry window discovery next time.
    if (g_extent.width < 800 || g_extent.height < 600) {
        LOG("derive: extent %ux%u looks implausible for a game window - retrying next frame.",
            g_extent.width, g_extent.height);
        g_hwnd = nullptr;
        g_bestWndArea = 0;
        g_format = VK_FORMAT_UNDEFINED;
        return false;
    }
    return g_extent.width > 0 && g_extent.height > 0;
}

static VkResult VKAPI_PTR hkAcquire(VkDevice device, VkSwapchainKHR sc, uint64_t to,
                                    VkSemaphore sem, VkFence fen, uint32_t* pIndex) {
    if (!g_device) { g_device = device; LOG_DEBUG("CAPTURED device via acquire = %p", (void*)device); }
    if (g_swapchain && g_swapchain != sc)
        destroy_swapchain_resources("acquire observed a new swapchain", VK_SUCCESS);
    g_swapchain = sc;
    VkResult result = oAcquire(device, sc, to, sem, fen, pIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        destroy_swapchain_resources("vkAcquireNextImageKHR", result);
    return result;
}

static bool load_device_funcs() {
    #define LOADFN(var, name) \
        var = (decltype(var))g_gipa(g_dummyInstance, name); \
        if (!var) { LOG("ERROR: could not load %s", name); return false; }
    LOADFN(pvkGetSwapchainImagesKHR, "vkGetSwapchainImagesKHR");
    LOADFN(pvkCreateImageView, "vkCreateImageView");
    LOADFN(pvkCreateRenderPass, "vkCreateRenderPass");
    LOADFN(pvkCreateFramebuffer, "vkCreateFramebuffer");
    LOADFN(pvkCreateCommandPool, "vkCreateCommandPool");
    LOADFN(pvkAllocateCommandBuffers, "vkAllocateCommandBuffers");
    LOADFN(pvkBeginCommandBuffer, "vkBeginCommandBuffer");
    LOADFN(pvkCmdBeginRenderPass, "vkCmdBeginRenderPass");
    LOADFN(pvkCmdEndRenderPass, "vkCmdEndRenderPass");
    LOADFN(pvkEndCommandBuffer, "vkEndCommandBuffer");
    LOADFN(pvkQueueSubmit, "vkQueueSubmit");
    LOADFN(pvkQueueWaitIdle, "vkQueueWaitIdle");
    LOADFN(pvkResetCommandBuffer, "vkResetCommandBuffer");
    LOADFN(pvkCreateFence, "vkCreateFence");
    LOADFN(pvkWaitForFences, "vkWaitForFences");
    LOADFN(pvkResetFences, "vkResetFences");
    LOADFN(pvkCreateSemaphore, "vkCreateSemaphore");
    LOADFN(pvkDeviceWaitIdle, "vkDeviceWaitIdle");
    LOADFN(pvkDestroyFramebuffer, "vkDestroyFramebuffer");
    LOADFN(pvkDestroyImageView, "vkDestroyImageView");
    LOADFN(pvkDestroyRenderPass, "vkDestroyRenderPass");
    LOADFN(pvkDestroyCommandPool, "vkDestroyCommandPool");
    LOADFN(pvkCreateQueryPool, "vkCreateQueryPool");
    LOADFN(pvkCmdResetQueryPool, "vkCmdResetQueryPool");
    LOADFN(pvkCmdWriteTimestamp, "vkCmdWriteTimestamp");
    LOADFN(pvkGetQueryPoolResults, "vkGetQueryPoolResults");
    LOADFN(pvkGetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
    LOADFN(pvkGetPhysicalDeviceQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties");
    #undef LOADFN
    return true;
}

static void destroy_swapchain_resources(const char* reason, VkResult status) {
    g_resourcesReady = false;
    // The command buffers are about to go; the timestamps they wrote have no owner any more.
    for (uint32_t i = 0; i < MAX_IMAGES; ++i) g_gpuTimingWritten[i] = false;

    bool hadResources = g_imguiInited || g_renderPass || g_cmdPool || g_imageCount > 0;
    if (hadResources && g_device && pvkDeviceWaitIdle) {
        VkResult waitResult = pvkDeviceWaitIdle(g_device);
        if (waitResult != VK_SUCCESS)
            LOG_ERROR("recreate: vkDeviceWaitIdle failed (result=%d).", (int)waitResult);
    }

    if (g_imguiInited) {
        ImGui_ImplVulkan_Shutdown();
        g_imguiInited = false;
    }
    if (g_device) {
        for (uint32_t i = 0; i < MAX_IMAGES; ++i) {
            if (g_framebuffers[i] && pvkDestroyFramebuffer)
                pvkDestroyFramebuffer(g_device, g_framebuffers[i], nullptr);
            if (g_views[i] && pvkDestroyImageView)
                pvkDestroyImageView(g_device, g_views[i], nullptr);
            g_framebuffers[i] = VK_NULL_HANDLE;
            g_views[i] = VK_NULL_HANDLE;
            g_cmds[i] = VK_NULL_HANDLE;
        }
        if (g_cmdPool && pvkDestroyCommandPool)
            pvkDestroyCommandPool(g_device, g_cmdPool, nullptr);
        if (g_renderPass && pvkDestroyRenderPass)
            pvkDestroyRenderPass(g_device, g_renderPass, nullptr);
    }
    g_cmdPool = VK_NULL_HANDLE;
    g_renderPass = VK_NULL_HANDLE;
    g_imageCount = 0;
    g_format = VK_FORMAT_UNDEFINED;
    g_extent = {0, 0};
    g_minImageCount = 2;

    LOG("recreate: overlay swapchain resources reset (reason=%s result=%d hadResources=%d).",
        reason ? reason : "unknown", (int)status, (int)hadResources);
}

static bool build_resources() {
    if (g_extent.width == 0 || g_extent.height == 0) {
        LOG_DEBUG("build_resources: zero extent; overlay deferred.");
        return false;
    }
    // Render pass: LOAD the game's frame, draw over it, keep PRESENT_SRC layout.
    VkAttachmentDescription att{};
    att.format = g_format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1; rp.pAttachments = &att;
    rp.subpassCount = 1; rp.pSubpasses = &sub;
    rp.dependencyCount = 1; rp.pDependencies = &dep;
    if (pvkCreateRenderPass(g_device, &rp, nullptr, &g_renderPass) != VK_SUCCESS) {
        LOG("ERROR: vkCreateRenderPass failed"); return false;
    }

    // Swapchain images.
    uint32_t count = 0;
    pvkGetSwapchainImagesKHR(g_device, g_swapchain, &count, nullptr);
    if (count == 0 || count > MAX_IMAGES) { LOG("ERROR: bad swapchain image count %u", count); return false; }
    VkImage images[MAX_IMAGES] = {};
    pvkGetSwapchainImagesKHR(g_device, g_swapchain, &count, images);
    g_imageCount = count;
    LOG("Swapchain images: %u, extent %ux%u, format %d", count, g_extent.width, g_extent.height, (int)g_format);

    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo iv{};
        iv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv.image = images[i];
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format = g_format;
        iv.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
        iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (pvkCreateImageView(g_device, &iv, nullptr, &g_views[i]) != VK_SUCCESS) {
            LOG("ERROR: vkCreateImageView[%u] failed", i); return false;
        }
        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = g_renderPass;
        fb.attachmentCount = 1; fb.pAttachments = &g_views[i];
        fb.width = g_extent.width; fb.height = g_extent.height; fb.layers = 1;
        if (pvkCreateFramebuffer(g_device, &fb, nullptr, &g_framebuffers[i]) != VK_SUCCESS) {
            LOG("ERROR: vkCreateFramebuffer[%u] failed", i); return false;
        }
    }

    // Command pool + one command buffer per image.
    VkCommandPoolCreateInfo cp{};
    cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = g_gfxFamily;
    if (pvkCreateCommandPool(g_device, &cp, nullptr, &g_cmdPool) != VK_SUCCESS) {
        LOG("ERROR: vkCreateCommandPool failed"); return false;
    }
    VkCommandBufferAllocateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb.commandPool = g_cmdPool;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = count;
    if (pvkAllocateCommandBuffers(g_device, &cb, g_cmds) != VK_SUCCESS) {
        LOG("ERROR: vkAllocateCommandBuffers failed"); return false;
    }

    // Per-image sync objects: created once, reused across any later swapchain recreation. Fences
    // start SIGNALED so the very first wait-before-record on each image returns immediately.
    if (!g_syncCreated) {
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (uint32_t i = 0; i < MAX_IMAGES; ++i) {
            if (pvkCreateFence(g_device, &fi, nullptr, &g_fences[i]) != VK_SUCCESS) {
                LOG("ERROR: vkCreateFence[%u] failed", i); return false;
            }
            if (pvkCreateSemaphore(g_device, &si, nullptr, &g_renderSem[i]) != VK_SUCCESS) {
                LOG("ERROR: vkCreateSemaphore[%u] failed", i); return false;
            }
        }
        g_syncCreated = true;
        LOG("Per-image sync objects created (%u fences + semaphores).", MAX_IMAGES);
    }

    // GPU timing, created once for the same reason the sync objects are: it depends on the
    // device, not on the swapchain's format or size. Everything here is optional - a card or a
    // queue that cannot do timestamps still gets an overlay, it just reports no GPU number
    // rather than an invented one.
    if (!g_gpuTimingReady && g_queryPool == VK_NULL_HANDLE) {
        VkPhysicalDeviceProperties props{};
        pvkGetPhysicalDeviceProperties(g_physicalDevice, &props);
        uint32_t familyCount = 0;
        pvkGetPhysicalDeviceQueueFamilyProperties(g_physicalDevice, &familyCount, nullptr);
        uint32_t validBits = 0;
        if (familyCount > 0 && familyCount <= 32) {
            VkQueueFamilyProperties families[32]{};
            pvkGetPhysicalDeviceQueueFamilyProperties(g_physicalDevice, &familyCount, families);
            if (g_gfxFamily < familyCount) validBits = families[g_gfxFamily].timestampValidBits;
        }
        if (props.limits.timestampPeriod <= 0.0f || validBits == 0) {
            LOG("GPU timing unavailable: timestampPeriod=%.3f validBits=%u. The overlay will "
                "report no GPU cost.", props.limits.timestampPeriod, validBits);
        } else {
            VkQueryPoolCreateInfo qp{};
            qp.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qp.queryCount = MAX_IMAGES * 2;
            if (pvkCreateQueryPool(g_device, &qp, nullptr, &g_queryPool) != VK_SUCCESS) {
                LOG_WARN("vkCreateQueryPool failed; no GPU cost will be reported.");
                g_queryPool = VK_NULL_HANDLE;
            } else {
                g_timestampPeriod = (double)props.limits.timestampPeriod;
                g_timestampMask = validBits >= 64 ? ~0ull : ((1ull << validBits) - 1ull);
                for (uint32_t i = 0; i < MAX_IMAGES; ++i) g_gpuTimingWritten[i] = false;
                g_gpuTimingReady = true;
                LOG("GPU timing ready: %.3f ns per tick, %u valid bits.",
                    g_timestampPeriod, validBits);
            }
        }
    }
    LOG("Vulkan overlay resources built OK.");
    return true;
}

static PFN_vkVoidFunction imgui_loader(const char* name, void* user) {
    return g_gipa((VkInstance)user, name);
}

// The sizes baked into the atlas. Rebuilding it cannot happen mid-frame, so a document asks
// for whatever size it likes and is drawn from the nearest of these, scaled. Five rather than
// the two there used to be: with only 20 and 32 baked, a 34px heading came out of the 20 and
// looked like mud, and there was no size a modder could pick that was certain to be sharp.
//
// A ladder, not a rule. Sizes a modder actually writes cluster low - captions and labels - so
// the steps are close down there and spread out at the top. More faces would cost atlas
// memory inside somebody's game for sharpness nobody would see.
static const float kFontLadder[] = {14.0f, 16.0f, 20.0f, 26.0f, 34.0f};
static const int kFontLadderCount = (int)(sizeof(kFontLadder) / sizeof(kFontLadder[0]));
static oreoui::FontFace g_faces[kFontLadderCount + 1];
static int g_faceCount = 0;

static bool init_imgui() {
    if (!ImGui::GetCurrentContext()) {
        ImGui::SetAllocatorFunctions(ImGuiAllocFn, ImGuiFreeFn);  // track overlay-owned memory
        ImGui::CreateContext();
        ImGuiIO& initIo = ImGui::GetIO();
        initIo.IniFilename = nullptr;
        // ImGui never changes the game's own cursor. It draws one of its own, but only while a
        // screen with widgets is up and the game could not be asked to show its own - see
        // render_overlay.
        initIo.MouseDrawCursor = false;
        initIo.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        // Widgets can be walked with the pad. Not with the keyboard: arrows, space and enter
        // belong to the game, and a panel that moved its focus every time the player walked
        // would be worse than no keyboard navigation at all.
        initIo.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

        // Mouse and keyboard, from the game's own window. Done once, with the context: the
        // Vulkan side is torn down and rebuilt with every swapchain, the window is not.
        if (ImGui_ImplWin32_Init(g_hwnd)) {
            // The pad is ours rather than the backend's; see kPadKeys.
            initIo.BackendFlags |= ImGuiBackendFlags_HasGamepad;
            InterlockedExchange(&g_inputReady, 1);
            // Read first, swap second: the window thread can deliver a message the instant the
            // swap lands, and by then hkWndProc must already know where to pass it on.
            g_gameWndProc = (WNDPROC)GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC);
            if (g_gameWndProc &&
                SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc) != 0) {
                LOG("input: mouse and keyboard reach the overlay (window %p).", (void*)g_hwnd);
            } else {
                LOG_ERROR("input: could not attach to the game window (winerror=%lu); widgets "
                          "will not see the mouse or keyboard.", GetLastError());
            }
        } else {
            LOG_ERROR("input: ImGui_ImplWin32_Init failed; widgets will not see the mouse or "
                      "keyboard.");
        }

        // The font ships with the Runtime, so OREO_RUNTIME_DIR is where it is. There used to be
        // a second lookup under the mod's own folder, left from when the overlay lived inside
        // the mod - a path no new installation has ever had, which only made it harder to see
        // which one was real. Windows fonts below are the genuine fallback.
        ImFont* base = initIo.Fonts->AddFontDefault();
        char assetDir[MAX_PATH] = {0};
        DWORD assetDirLength = GetEnvironmentVariableA("OREO_RUNTIME_DIR", assetDir, MAX_PATH);
        std::string bodyPath;
        if (assetDirLength > 0 && assetDirLength < MAX_PATH)
            bodyPath = std::string(assetDir) + "\\assets\\fonts\\gamefont.ttf";
        char windowsDir[MAX_PATH] = {0};
        UINT windowsDirLength = GetWindowsDirectoryA(windowsDir, MAX_PATH);
        std::string systemPath;
        if (windowsDirLength > 0 && windowsDirLength < MAX_PATH)
            systemPath = std::string(windowsDir) + "\\Fonts\\segoeui.ttf";

        for (int i = 0; i < kFontLadderCount; ++i) {
            ImFont* face = nullptr;
            if (!bodyPath.empty())
                face = initIo.Fonts->AddFontFromFileTTF(
                    bodyPath.c_str(), kFontLadder[i], nullptr,
                    initIo.Fonts->GetGlyphRangesCyrillic());
            if (!face && !systemPath.empty())
                face = initIo.Fonts->AddFontFromFileTTF(
                    systemPath.c_str(), kFontLadder[i], nullptr,
                    initIo.Fonts->GetGlyphRangesCyrillic());
            if (face) g_faces[g_faceCount++] = {face, kFontLadder[i]};
        }
        // Nothing loaded at all: the built-in bitmap font still draws, badly, which beats an
        // overlay that silently shows no text.
        if (g_faceCount == 0) g_faces[g_faceCount++] = {base, 13.0f};
        LOG("fonts loaded: %d of %d sizes, custom-or-system=%s",
            g_faceCount, kFontLadderCount, g_faces[0].font != base ? "yes" : "fallback-default");
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_extent.width, (float)g_extent.height);

    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0, imgui_loader, (void*)g_dummyInstance)) {
        LOG("ERROR: ImGui_ImplVulkan_LoadFunctions failed"); return false;
    }

    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_0;
    ii.Instance = g_dummyInstance;
    ii.PhysicalDevice = g_physicalDevice;
    ii.Device = g_device;
    ii.QueueFamily = g_gfxFamily;
    ii.Queue = g_queue;
    ii.DescriptorPoolSize = 16;                 // let imgui manage its own descriptor pool
    ii.MinImageCount = g_minImageCount < 2 ? 2 : g_minImageCount;
    ii.ImageCount = g_imageCount;
    ii.PipelineInfoMain.RenderPass = g_renderPass;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&ii)) {
        LOG("ERROR: ImGui_ImplVulkan_Init failed"); return false;
    }
    LOG(">>> imgui initialized. <<<");
    return true;
}

// ---- what the overlay costs -------------------------------------------------------------
//
// The overlay's own diagnostics, drawn by the overlay itself - not published into a mod's
// document. It used to be the other way round: these were bound `runtime.*` values a mod's
// UI file had to describe on its own screen. That made the overlay's health someone else's
// file to maintain, and every mod that wanted to show it had to lay the same panel out by
// hand. Decided 29 Aug 2026: this is diagnostics about the overlay, not content from any mod,
// so it lives and draws where the overlay lives.
//
// Gated by a setting that belongs to the overlay, not to any mod - see
// refresh_overlay_debug_flag() below.

static LARGE_INTEGER g_qpcFreq = {};
static double g_lastT = -1.0;

static double g_frameCostMs = 0.0;
static double g_gpuCostMs = 0.0;

// One number jumps around and says nothing, so each cost keeps a short history and reports its
// average and its worst case alongside the current value. Two seconds at 60 Hz: long enough to
// be readable, short enough that a stutter is still visible in it rather than averaged away.
static const int kCostSamples = 120;

struct CostHistory {
    double samples[kCostSamples] = {};
    int index = 0;
    int filled = 0;

    void add(double value) {
        samples[index] = value;
        index = (index + 1) % kCostSamples;
        if (filled < kCostSamples) ++filled;
    }
    double average() const {
        if (filled == 0) return 0.0;
        double total = 0.0;
        for (int i = 0; i < filled; ++i) total += samples[i];
        return total / filled;
    }
    double worst() const {
        double top = 0.0;
        for (int i = 0; i < filled; ++i) if (samples[i] > top) top = samples[i];
        return top;
    }
};

static CostHistory g_cpuCost;
static CostHistory g_gpuCost;

// What the pipe is actually carrying. Written by the pipe thread, read by the render thread;
// plain counters, so they are read through Interlocked rather than assumed to be atomic.
static volatile LONG64 g_wireMessages = 0;
static volatile LONG64 g_wireBytes = 0;

static oreoui::DrawStats g_lastDrawStats;

// Diagnostics are published four times a second, not sixty. Nobody can read a number that
// changes every frame, the averages above are what carries the detail, and this keeps the
// document's lock out of the hot path for fifty-odd frames out of every sixty.
static ULONGLONG g_lastPublishTick = 0;
static LONG64 g_lastWireMessages = 0;
static LONG64 g_lastWireBytes = 0;

// %APPDATA%\ProjectOreo\overlay.ini, same [Debug] DebugHotkeys=true shape the launcher and
// the mod already use. Not %LOCALAPPDATA%\ProjectOreo, which the Runtime has never created and
// still does not for anything else - this is the one setting that is genuinely the overlay's
// own, not routed through any mod's folder.
static std::string overlay_ini_path() {
    char appdata[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::string();
    return std::string(appdata) + "\\ProjectOreo\\overlay.ini";
}

static bool g_overlayDebug = false;
static ULONGLONG g_overlayDebugCheckedTick = 0;

// Re-read every couple of seconds rather than once at startup, so turning this on or off does
// not need a game restart - the same reasoning as log.h re-checking its own settings lazily.
static void refresh_overlay_debug_flag() {
    ULONGLONG now = GetTickCount64();
    if (g_overlayDebugCheckedTick != 0 && now - g_overlayDebugCheckedTick < 2000) return;
    g_overlayDebugCheckedTick = now;
    std::string path = overlay_ini_path();
    if (path.empty()) { g_overlayDebug = false; return; }
    // GetPrivateProfileIntA parses decimal/hex/binary only - "true" is none of those and
    // silently reads back as 0, the same as the key being absent. Every other true/false
    // setting in this codebase (log.h, the launcher's config.cpp) is a word, not a digit, and
    // this one has to match that or nobody types the value that actually works.
    char value[16] = {0};
    GetPrivateProfileStringA("Debug", "DebugHotkeys", "false", value, sizeof(value), path.c_str());
    g_overlayDebug = _stricmp(value, "true") == 0 || _stricmp(value, "1") == 0;
}

// Formatted once every 250ms and drawn every frame from the cache. Nobody can read a number
// that changes every frame - the averages above already carry the detail - and recomputing the
// memory query and the wire rate that often would be paying a cost just to measure a cost.
struct OverlayDiagnostics {
    char frameMs[32] = "0.000", frameMsAvg[32] = "0.000", frameMsMax[32] = "0.000";
    char gpuMs[32] = "n/a", gpuMsAvg[32] = "n/a", gpuMsMax[32] = "n/a";
    char imguiKB[32] = "0", imguiAllocs[32] = "0", processMB[32] = "0.0";
    char wireMsgPerSec[32] = "0", wireKBPerSec[32] = "0.0";
    char elementsDrawn[32] = "0", elementsHidden[32] = "0", screensVisible[32] = "0";
};
static OverlayDiagnostics g_diag;

static void refresh_overlay_diagnostics() {
    ULONGLONG now = GetTickCount64();
    if (g_lastPublishTick != 0 && now - g_lastPublishTick < 250) return;
    ULONGLONG elapsed = g_lastPublishTick == 0 ? 0 : now - g_lastPublishTick;
    g_lastPublishTick = now;

    LONG64 messages = InterlockedCompareExchange64(&g_wireMessages, 0, 0);
    LONG64 bytes = InterlockedCompareExchange64(&g_wireBytes, 0, 0);
    double perSecond = elapsed > 0 ? 1000.0 / (double)elapsed : 0.0;
    double messageRate = (double)(messages - g_lastWireMessages) * perSecond;
    double byteRate = (double)(bytes - g_lastWireBytes) * perSecond;
    g_lastWireMessages = messages;
    g_lastWireBytes = bytes;

    PROCESS_MEMORY_COUNTERS pmc{};
    double processMB = 0.0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        processMB = (double)pmc.WorkingSetSize / (1024.0 * 1024.0);

    snprintf(g_diag.frameMs, sizeof(g_diag.frameMs), "%.3f", g_frameCostMs);
    snprintf(g_diag.frameMsAvg, sizeof(g_diag.frameMsAvg), "%.3f", g_cpuCost.average());
    snprintf(g_diag.frameMsMax, sizeof(g_diag.frameMsMax), "%.3f", g_cpuCost.worst());
    if (g_gpuTimingReady) {
        snprintf(g_diag.gpuMs, sizeof(g_diag.gpuMs), "%.3f", g_gpuCostMs);
        snprintf(g_diag.gpuMsAvg, sizeof(g_diag.gpuMsAvg), "%.3f", g_gpuCost.average());
        snprintf(g_diag.gpuMsMax, sizeof(g_diag.gpuMsMax), "%.3f", g_gpuCost.worst());
    } else {
        // Said out loud rather than left at zero. "0.000 ms of GPU" is a claim; "n/a" is the
        // truth, and whoever is reading it can tell the two apart.
        strcpy_s(g_diag.gpuMs, "n/a");
        strcpy_s(g_diag.gpuMsAvg, "n/a");
        strcpy_s(g_diag.gpuMsMax, "n/a");
    }
    snprintf(g_diag.imguiKB, sizeof(g_diag.imguiKB), "%.0f", (double)g_imguiLiveBytes / 1024.0);
    snprintf(g_diag.imguiAllocs, sizeof(g_diag.imguiAllocs), "%.0f", (double)g_imguiLiveAllocs);
    snprintf(g_diag.processMB, sizeof(g_diag.processMB), "%.1f", processMB);
    snprintf(g_diag.wireMsgPerSec, sizeof(g_diag.wireMsgPerSec), "%.0f", messageRate);
    snprintf(g_diag.wireKBPerSec, sizeof(g_diag.wireKBPerSec), "%.1f", byteRate / 1024.0);
    snprintf(g_diag.elementsDrawn, sizeof(g_diag.elementsDrawn), "%.0f",
             (double)g_lastDrawStats.drawn);
    snprintf(g_diag.elementsHidden, sizeof(g_diag.elementsHidden), "%.0f",
             (double)g_lastDrawStats.hidden);
    snprintf(g_diag.screensVisible, sizeof(g_diag.screensVisible), "%.0f",
             (double)g_lastDrawStats.screens_visible);
}

// Top-right corner, opposite whatever a mod's own settings screen puts in the top-left. The
// two are unrelated documents - this one is not part of any mod's UI file at all - and have no
// reason to compete for the same corner.
static void draw_overlay_diagnostics(ImDrawList* dl, float screen_w, float screen_h,
                                     ImFont* font, ImFont* bigFont) {
    (void)screen_h;  // the panel pins to the top-right corner; its height never depends on this
    refresh_overlay_diagnostics();

    const float width = 300.0f, pad = 14.0f;
    const float fontSize = 14.0f, lineH = 20.0f;
    const int lineCount = 14;
    const float height = 44.0f + lineCount * lineH + pad;
    const float x = screen_w - width - 20.0f, y = 20.0f;
    ImFont* bodyFont = font ? font : ImGui::GetFont();
    ImFont* titleFont = bigFont ? bigFont : bodyFont;
    const ImU32 bg = IM_COL32(4, 7, 12, 235);
    const ImU32 border = IM_COL32(125, 220, 255, 255);
    const ImU32 label = IM_COL32(150, 170, 190, 255);
    const ImU32 value = IM_COL32(230, 240, 250, 255);

    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height), bg, 8.0f);
    dl->AddRect(ImVec2(x, y), ImVec2(x + width, y + height), border, 8.0f, 0, 1.5f);
    dl->AddText(titleFont, 20.0f, ImVec2(x + pad, y + 10.0f), border, "OVERLAY");

    float cy = y + 44.0f;
    auto row = [&](const char* name, const char* val) {
        dl->AddText(bodyFont, fontSize, ImVec2(x + pad, cy), label, name);
        dl->AddText(bodyFont, fontSize, ImVec2(x + width - pad - 70.0f, cy), value, val);
        cy += lineH;
    };
    row("CPU MS", g_diag.frameMs);
    row("CPU MS AVG", g_diag.frameMsAvg);
    row("CPU MS MAX", g_diag.frameMsMax);
    row("GPU MS", g_diag.gpuMs);
    row("GPU MS AVG", g_diag.gpuMsAvg);
    row("GPU MS MAX", g_diag.gpuMsMax);
    row("OVERLAY KB", g_diag.imguiKB);
    row("OVERLAY ALLOCS", g_diag.imguiAllocs);
    row("PROCESS MB", g_diag.processMB);
    row("PIPE MSG/S", g_diag.wireMsgPerSec);
    row("PIPE KB/S", g_diag.wireKBPerSec);
    row("ELEMENTS DRAWN", g_diag.elementsDrawn);
    row("ELEMENTS HIDDEN", g_diag.elementsHidden);
    row("SCREENS VISIBLE", g_diag.screensVisible);
}

// Records + submits the overlay draw for this image and returns the semaphore that the caller's
// present must wait on (our draw-complete). Returns VK_NULL_HANDLE only if it did not submit.
static VkSemaphore render_overlay(uint32_t imageIndex, const VkPresentInfoKHR* pi) {
    ImGuiIO& io = ImGui::GetIO();

    if (g_qpcFreq.QuadPart == 0) QueryPerformanceFrequency(&g_qpcFreq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    double t = (double)c.QuadPart / g_qpcFreq.QuadPart;
    float dt = (g_lastT < 0) ? (1.0f / 60.0f) : (float)(t - g_lastT);
    g_lastT = t;
    if (dt > 0.1f) dt = 0.1f; if (dt < 0) dt = 0;

    // Measured around the whole overlay frame. Worth keeping now more than before: the mod
    // used to send one line per frame and now sends a handful, and this is the number that
    // says whether that ever matters.
    LARGE_INTEGER fcStart, fcEnd;
    QueryPerformanceCounter(&fcStart);

    // Starting a frame is where ImGui consumes its input queue, so it happens under the same
    // lock the window procedure feeds that queue under. The backend sets its own display size
    // and frame time from the window; ours, from the swapchain, are set after and win.
    EnterCriticalSection(&g_inputLock);
    if (InterlockedCompareExchange(&g_inputReady, 0, 0)) ImGui_ImplWin32_NewFrame();
    io.DisplaySize = ImVec2((float)g_extent.width, (float)g_extent.height);
    io.DeltaTime = dt > 0 ? dt : 1.0f / 60.0f;
    ImGui::NewFrame();
    LeaveCriticalSection(&g_inputLock);

    // The whole of what this overlay draws. Screens, elements, positions, colours and text
    // all came down the pipe from a mod; nothing here knows what any of it means.
    g_lastDrawStats = oreoui::DrawStats();
    if (!g_ui.empty()) {
        std::vector<std::string> events;
        g_lastDrawStats = g_ui.draw((float)g_extent.width, (float)g_extent.height,
                                    g_faces, g_faceCount, events);
        if (!events.empty()) {
            std::lock_guard<std::mutex> guard(g_outboxLock);
            // With no mod connected nothing drains this; a player clicking at a panel left
            // behind by a mod that went away must not grow it forever.
            if (g_outbox.size() > 4096) g_outbox.clear();
            for (std::string& line : events) g_outbox.push_back(std::move(line));
        }
    }
    // A panel with widgets takes the player's input away from the game and has it show the
    // pointer; see follow_panel.
    bool gameShowsPointer = follow_panel(g_lastDrawStats.interactive);
    // Our own pointer, while there is something to point at, if the game could not be asked
    // to show its own. The game hides the system cursor whenever the camera follows the mouse -
    // third person, most of the time - and a panel of buttons with nothing showing where the
    // mouse is cannot be used. ImGui draws it at the end of Render, above everything, where the
    // mouse actually is.
    io.MouseDrawCursor = g_lastDrawStats.interactive && !gameShowsPointer;
    // The overlay's own panel, unrelated to whatever a mod just drew above. Gated by its own
    // file, re-checked every couple of seconds so flipping it needs no restart.
    refresh_overlay_debug_flag();
    if (g_overlayDebug)
        draw_overlay_diagnostics(ImGui::GetForegroundDrawList(),
                                 (float)g_extent.width, (float)g_extent.height,
                                 g_faceCount > 0 ? g_faces[0].font : nullptr,
                                 g_faceCount > 0 ? g_faces[g_faceCount - 1].font : nullptr);
    ImGui::Render();

    QueryPerformanceCounter(&fcEnd);
    g_frameCostMs = (double)(fcEnd.QuadPart - fcStart.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
    g_cpuCost.add(g_frameCostMs);

    // Wait for this image's previous overlay submit before reusing its command buffer.
    pvkWaitForFences(g_device, 1, &g_fences[imageIndex], VK_TRUE, UINT64_MAX);
    pvkResetFences(g_device, 1, &g_fences[imageIndex]);

    // That fence also means the timestamps this image wrote last time round are finished, so
    // they can be collected without waiting on anything. A frame or two of lag on a diagnostic
    // number is invisible; a stall to read it would be the very cost we set out to measure.
    if (g_gpuTimingReady && g_gpuTimingWritten[imageIndex]) {
        uint64_t stamps[2] = {0, 0};
        VkResult queryResult = pvkGetQueryPoolResults(
            g_device, g_queryPool, imageIndex * 2, 2, sizeof(stamps), stamps,
            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (queryResult == VK_SUCCESS) {
            uint64_t begin = stamps[0] & g_timestampMask;
            uint64_t end = stamps[1] & g_timestampMask;
            if (end >= begin) {
                g_gpuCostMs = (double)(end - begin) * g_timestampPeriod / 1000000.0;
                g_gpuCost.add(g_gpuCostMs);
            }
        }
    }

    VkCommandBuffer cmd = g_cmds[imageIndex];
    pvkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    pvkBeginCommandBuffer(cmd, &bi);

    // Resetting a query pool has to happen outside a render pass, hence here rather than
    // around the draw itself.
    if (g_gpuTimingReady) {
        pvkCmdResetQueryPool(cmd, g_queryPool, imageIndex * 2, 2);
        pvkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_queryPool, imageIndex * 2);
    }

    VkRenderPassBeginInfo rpb{};
    rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpb.renderPass = g_renderPass;
    rpb.framebuffer = g_framebuffers[imageIndex];
    rpb.renderArea.extent = g_extent;
    pvkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    pvkCmdEndRenderPass(cmd);
    if (g_gpuTimingReady) {
        pvkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_queryPool,
                             imageIndex * 2 + 1);
        g_gpuTimingWritten[imageIndex] = true;
    }
    pvkEndCommandBuffer(cmd);

    // GPU-side ordering without a stall: our submit waits on the SAME render-finished semaphores
    // that present was going to wait on (so we draw only after the game finished the image), and
    // signals our own semaphore. The caller rewrites present to wait on ours instead. Chain:
    // game-render -> overlay-draw -> present, all on the GPU.
    VkSemaphore      waitSems[8];
    VkPipelineStageFlags waitStages[8];
    uint32_t waitCount = 0;
    if (pi && pi->waitSemaphoreCount > 0 && pi->pWaitSemaphores) {
        waitCount = pi->waitSemaphoreCount < 8 ? pi->waitSemaphoreCount : 8;
        for (uint32_t i = 0; i < waitCount; ++i) {
            waitSems[i]   = pi->pWaitSemaphores[i];
            waitStages[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        }
    } else {
        // No present semaphore to chain against (not expected for NMS - engines always present
        // with a render-finished semaphore). Conservative fallback so we never draw over an
        // unfinished frame; this stall branch is effectively never taken in NMS.
        pvkQueueWaitIdle(g_queue);
    }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = waitCount;
    si.pWaitSemaphores = waitCount ? waitSems : nullptr;
    si.pWaitDstStageMask = waitCount ? waitStages : nullptr;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g_renderSem[imageIndex];
    pvkQueueSubmit(g_queue, 1, &si, g_fences[imageIndex]);
    return g_renderSem[imageIndex];
}

static VkResult VKAPI_PTR hkCreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* ci,
                                            const VkAllocationCallbacks* alloc, VkSwapchainKHR* sc) {
    VkResult r = oCreateSwapchain(device, ci, alloc, sc);
    if (r == VK_SUCCESS && ci) {
        destroy_swapchain_resources("vkCreateSwapchainKHR", r);
        g_device = device;
        g_swapchain = *sc;
        LOG_DEBUG("CAPTURED new swapchain: device=%p requestedFormat=%d requestedExtent=%ux%u "
                  "requestedMinImages=%u; surface state will be queried again",
            (void*)device, (int)ci->imageFormat, ci->imageExtent.width, ci->imageExtent.height,
            ci->minImageCount);
    }
    return r;
}

static bool game_window_extent_is_zero() {
    if (!g_hwnd) return false;
    RECT rect{};
    if (!GetClientRect(g_hwnd, &rect)) return false;
    return rect.right <= rect.left || rect.bottom <= rect.top;
}

static VkResult present_original_and_track(VkQueue queue, const VkPresentInfoKHR* pi) {
    VkResult result = oPresent(queue, pi);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        destroy_swapchain_resources("vkQueuePresentKHR", result);
    return result;
}

static VkResult hkPresentBody(VkQueue queue, const VkPresentInfoKHR* pi) {
    g_queue = queue;
    if (pi && pi->swapchainCount > 0 && pi->pSwapchains) {
        VkSwapchainKHR presentedSwapchain = pi->pSwapchains[0];
        if (g_swapchain && g_swapchain != presentedSwapchain)
            destroy_swapchain_resources("present observed a new swapchain", VK_SUCCESS);
        g_swapchain = presentedSwapchain;
    }

    // A minimized GLFW window has a zero client extent. Do not record a render pass or create
    // framebuffers until it is restored; the game's present still runs unchanged.
    if (game_window_extent_is_zero())
        return present_original_and_track(queue, pi);

    // Need the game's device (from acquire hook). Format/extent via surface query.
    if (g_device && g_swapchain) {
        if (g_format == VK_FORMAT_UNDEFINED) { derive_format_extent(); }

        if (g_format != VK_FORMAT_UNDEFINED && g_extent.width && g_extent.height && !g_resourcesReady) {
            static bool loaded = false;
            if (!loaded) { loaded = load_device_funcs(); }
            if (loaded) {
                bool built = build_resources();
                if (built && !g_imguiInited) g_imguiInited = init_imgui();
                g_resourcesReady = built && g_imguiInited;
                if (!g_resourcesReady) {
                    LOG_ERROR("present: build_resources/init failed; resources will be queried again.");
                    destroy_swapchain_resources("resource build failure", VK_ERROR_INITIALIZATION_FAILED);
                }
            }
        }
        if (g_resourcesReady && pi) {
            uint32_t idx = pi->pImageIndices ? pi->pImageIndices[0] : 0;
            if (idx < g_imageCount) {
                VkSemaphore sem = render_overlay(idx, pi);
                if (sem != VK_NULL_HANDLE) {
                    // Present now waits on our overlay-draw semaphore instead of the game's
                    // render-finished ones (which our submit already consumed). Copy the rest of
                    // the present info verbatim so swapchains / image indices / results stay intact.
                    VkPresentInfoKHR mod = *pi;
                    mod.waitSemaphoreCount = 1;
                    mod.pWaitSemaphores = &sem;
                    return present_original_and_track(queue, &mod);
                }
            }
        }
    }
    return present_original_and_track(queue, pi);
}

static int present_exception_filter(unsigned int code) {
    g_presentFaulted = true;
    g_resourcesReady = false;
    give_up_input();
    LOG_ERROR("SEH exception in hkPresent body: code=0x%08X; overlay disabled for this process.", code);
    return EXCEPTION_EXECUTE_HANDLER;
}

static VkResult VKAPI_PTR hkPresent(VkQueue queue, const VkPresentInfoKHR* pi) {
    if (g_presentFaulted) return oPresent(queue, pi);
    __try {
        return hkPresentBody(queue, pi);
    } __except (present_exception_filter(GetExceptionCode())) {
    }
    return oPresent(queue, pi);
}

// ---- setup --------------------------------------------------------------------------
static void* resolve_trampolines_and_setup() {
    HMODULE vk = GetModuleHandleA("vulkan-1.dll");
    if (!vk) { LOG("ERROR: vulkan-1.dll not loaded."); return nullptr; }
    g_gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
    if (!g_gipa) { LOG("ERROR: no vkGetInstanceProcAddr."); return nullptr; }

    auto vkCreateInstance = (PFN_vkCreateInstance)g_gipa(nullptr, "vkCreateInstance");
    const char* instExts[] = { VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_win32_surface" };
    VkApplicationInfo app{}; app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici{}; ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app; ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = instExts;
    if (vkCreateInstance(&ici, nullptr, &g_dummyInstance) != VK_SUCCESS) {
        LOG("ERROR: vkCreateInstance failed with required Win32 surface extensions.");
        return nullptr;
    }
    auto vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)g_gipa(g_dummyInstance, "vkEnumeratePhysicalDevices");
    auto vkGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)g_gipa(g_dummyInstance, "vkGetPhysicalDeviceProperties");
    auto vkGetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)g_gipa(g_dummyInstance, "vkGetPhysicalDeviceQueueFamilyProperties");
    auto vkGetPhysicalDeviceSurfaceSupportKHR = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)g_gipa(g_dummyInstance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    auto vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)g_gipa(g_dummyInstance, "vkCreateWin32SurfaceKHR");
    auto vkDestroySurfaceKHR = (PFN_vkDestroySurfaceKHR)g_gipa(g_dummyInstance, "vkDestroySurfaceKHR");
    auto vkCreateDevice = (PFN_vkCreateDevice)g_gipa(g_dummyInstance, "vkCreateDevice");
    auto vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)g_gipa(g_dummyInstance, "vkGetDeviceProcAddr");
    auto vkDestroyDevice = (PFN_vkDestroyDevice)g_gipa(g_dummyInstance, "vkDestroyDevice");

    if (!vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceProperties ||
        !vkGetPhysicalDeviceQueueFamilyProperties || !vkGetPhysicalDeviceSurfaceSupportKHR ||
        !vkCreateWin32SurfaceKHR || !vkDestroySurfaceKHR || !vkCreateDevice || !vkGetDeviceProcAddr) {
        LOG("ERROR: required Vulkan setup functions are unavailable.");
        return nullptr;
    }

    if (!g_hwnd) { g_bestWndArea = 0; EnumWindows(enum_wnd, 0); }
    if (!g_hwnd) {
        LOG("ERROR: no game HWND available for physical-device surface selection.");
        return nullptr;
    }
    VkWin32SurfaceCreateInfoKHR selectionSurfaceInfo{};
    selectionSurfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    selectionSurfaceInfo.hinstance = GetModuleHandleA(nullptr);
    selectionSurfaceInfo.hwnd = g_hwnd;
    VkSurfaceKHR selectionSurface = VK_NULL_HANDLE;
    VkResult surfaceResult = vkCreateWin32SurfaceKHR(
        g_dummyInstance, &selectionSurfaceInfo, nullptr, &selectionSurface);
    if (surfaceResult != VK_SUCCESS) {
        LOG("ERROR: selection vkCreateWin32SurfaceKHR failed (result=%d).", (int)surfaceResult);
        return nullptr;
    }

    uint32_t gpuCount = 0;
    VkResult enumerateResult = vkEnumeratePhysicalDevices(g_dummyInstance, &gpuCount, nullptr);
    if (enumerateResult != VK_SUCCESS || !gpuCount) {
        LOG("ERROR: physical-device enumeration failed (result=%d count=%u).",
            (int)enumerateResult, gpuCount);
        vkDestroySurfaceKHR(g_dummyInstance, selectionSurface, nullptr);
        return nullptr;
    }
    VkPhysicalDevice gpus[8]{}; if (gpuCount > 8) gpuCount = 8;
    enumerateResult = vkEnumeratePhysicalDevices(g_dummyInstance, &gpuCount, gpus);
    if (enumerateResult != VK_SUCCESS && enumerateResult != VK_INCOMPLETE) {
        LOG("ERROR: physical-device enumeration fetch failed (result=%d).", (int)enumerateResult);
        vkDestroySurfaceKHR(g_dummyInstance, selectionSurface, nullptr);
        return nullptr;
    }

    uint32_t selectedIndex = UINT32_MAX;
    uint32_t selectedFamily = UINT32_MAX;
    bool selectedIsDiscrete = false;
    LOG("GPU inventory: enumerated=%u", gpuCount);
    for (uint32_t i = 0; i < gpuCount; ++i) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(gpus[i], &props);
        const char* vendor = props.vendorID == 0x10DE ? "NVIDIA" :
                             props.vendorID == 0x1002 ? "AMD" :
                             props.vendorID == 0x8086 ? "Intel" : "Other";
        const char* type = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete" :
                           props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated" :
                           props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? "virtual" :
                           props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? "cpu" : "other";
        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &qfCount, nullptr);
        VkQueueFamilyProperties qfs[16]{}; if (qfCount > 16) qfCount = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &qfCount, qfs);
        bool hasSurfaceSupport = false;
        uint32_t graphicsPresentFamily = UINT32_MAX;
        for (uint32_t q = 0; q < qfCount; ++q) {
            VkBool32 supported = VK_FALSE;
            VkResult supportResult = vkGetPhysicalDeviceSurfaceSupportKHR(
                gpus[i], q, selectionSurface, &supported);
            if (supportResult == VK_SUCCESS && supported) {
                hasSurfaceSupport = true;
                if ((qfs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && graphicsPresentFamily == UINT32_MAX)
                    graphicsPresentFamily = q;
            }
        }
        bool eligible = hasSurfaceSupport && graphicsPresentFamily != UINT32_MAX;
        // Deliberately do not log deviceUUID, driverUUID, LUID, serials, or any other unique IDs.
        LOG("GPU[%u] candidate: name=%s vendor=%s type=%s surfaceSupport=%s "
            "graphicsPresentFamily=%d eligible=%s vendorID=0x%04X deviceID=0x%04X "
            "vulkan=%u.%u.%u driverRaw=0x%08X",
            i, props.deviceName, vendor, type, hasSurfaceSupport ? "yes" : "no",
            graphicsPresentFamily == UINT32_MAX ? -1 : (int)graphicsPresentFamily,
            eligible ? "yes" : "no",
            props.vendorID, props.deviceID, VK_VERSION_MAJOR(props.apiVersion),
            VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion), props.driverVersion);

        bool isDiscrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        if (eligible && (selectedIndex == UINT32_MAX || (isDiscrete && !selectedIsDiscrete))) {
            selectedIndex = i;
            selectedFamily = graphicsPresentFamily;
            selectedIsDiscrete = isDiscrete;
        }
    }
    vkDestroySurfaceKHR(g_dummyInstance, selectionSurface, nullptr);

    if (selectedIndex == UINT32_MAX) {
        LOG("ERROR: no physical device has a graphics queue that supports the game surface.");
        return nullptr;
    }
    g_physicalDevice = gpus[selectedIndex];
    g_gfxFamily = selectedFamily;
    VkPhysicalDeviceProperties selectedProps{};
    vkGetPhysicalDeviceProperties(g_physicalDevice, &selectedProps);
    const char* selectedType = selectedProps.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete" :
                               selectedProps.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated" :
                               selectedProps.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? "virtual" :
                               selectedProps.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? "cpu" : "other";
    LOG("GPU selected: index=%u name=%s type=%s graphicsPresentFamily=%u",
        selectedIndex, selectedProps.deviceName, selectedType, g_gfxFamily);

    // Throwaway device just to get device trampolines for present + createswapchain.
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{}; qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g_gfxFamily; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    const char* devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci{}; dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = devExts;
    VkDevice dummyDev = VK_NULL_HANDLE;
    if (vkCreateDevice(g_physicalDevice, &dci, nullptr, &dummyDev) != VK_SUCCESS) {
        LOG("ERROR: dummy vkCreateDevice failed."); return nullptr;
    }
    void* present = (void*)vkGetDeviceProcAddr(dummyDev, "vkQueuePresentKHR");
    void* createSc = (void*)vkGetDeviceProcAddr(dummyDev, "vkCreateSwapchainKHR");
    void* acquire = (void*)vkGetDeviceProcAddr(dummyDev, "vkAcquireNextImageKHR");
    LOG_DEBUG("trampolines: present=%p createSwapchain=%p acquire=%p", present, createSc, acquire);
    if (vkDestroyDevice) vkDestroyDevice(dummyDev, nullptr);

    if (!present || !acquire) { LOG("ERROR: could not resolve present/acquire trampolines."); return nullptr; }

    if (MH_Initialize() != MH_OK) { LOG("ERROR: MH_Initialize failed."); return nullptr; }
    bool ok = MH_CreateHook(present, (LPVOID)&hkPresent, reinterpret_cast<LPVOID*>(&oPresent)) == MH_OK;
    ok = ok && MH_CreateHook(acquire, (LPVOID)&hkAcquire, reinterpret_cast<LPVOID*>(&oAcquire)) == MH_OK;
    if (createSc)
        ok = ok && MH_CreateHook(createSc, (LPVOID)&hkCreateSwapchain, reinterpret_cast<LPVOID*>(&oCreateSwapchain)) == MH_OK;
    if (!ok) { LOG("ERROR: MH_CreateHook failed."); return nullptr; }
    hook_game_input();
    MH_EnableHook(MH_ALL_HOOKS);
    LOG("Hooks enabled (present + acquire + createSwapchain). Overlay should appear shortly.");
    return present;
}

// Reads a float the way the sender wrote it. NOT strtod or sscanf: those follow LC_NUMERIC,
// and under a Russian or German locale "0.42" is read as 0 - a bug that never shows up on the
// machine it was written on, only on the player's.
static bool parse_float_c(const char* text, float& out) {
    while (*text == ' ') ++text;
    const char* end = text;
    while (*end && *end != ' ') ++end;
    auto result = std::from_chars(text, end, out);
    return result.ec == std::errc() && result.ptr == end;
}

// ---- named pipe server: the mod's interface arrives here ------------------------------
// Protocol (mod -> overlay), newline-terminated:
//   UI <json>            the whole interface; on connect and on every reconnect
//   SET <name> <number>  a bound value moved
//   TXT <name> <text>    a bound string changed
//   VIS <name> <0|1>     a screen or element was shown or hidden
//   POS <screen> <x> <y> [anchor]   a screen moved; this is what a "move the panel" key calls
//   RAW <screen> <json>  free-hand shapes drawn above that screen - the escape hatch
//   VAL <id> <json>      a widget's value, set by the mod: true, 0.5, "text"
//   OPT <id> <json>      a dropdown's choices, a JSON array of strings
//   ENA <id> <0|1>       a widget can or cannot be used
//   PAD <a,b,up,...>     the gamepad buttons held right now; sent by the Runtime, not the mod
//
// And back the other way:
//   HELLO ui-format <n>  sent the moment a mod connects
//   OK <what>            a document or a batch of raw shapes was accepted
//   ERR <what> <text>    ...or was not, with the reason
//   EV click <id>        the player pressed a button
//   EV change <id> <json>   ...changed a toggle, slider, dropdown or input
//   EV submit <id> <json>   ...pressed Enter in an input
//   INPUT <device>       the player switched to "gamepad" or "mouse_keyboard"; also on connect
//
// Per-frame traffic is never answered, which is what keeps the reverse direction from filling a
// buffer nobody is draining. Events are the player's doing rather than an answer, and at most a
// few per frame while a slider is dragged.

static HANDLE g_pipeHandle = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_pipeWriteLock;
static bool g_pipeWriteLockReady = false;

static void pipe_send(const char* line) {
    if (!g_pipeWriteLockReady) return;
    EnterCriticalSection(&g_pipeWriteLock);
    HANDLE pipe = g_pipeHandle;
    if (pipe != INVALID_HANDLE_VALUE) {
        std::string message = std::string(line) + "\n";
        DWORD written = 0;
        if (!WriteFile(pipe, message.c_str(), (DWORD)message.size(), &written, nullptr))
            LOG_DEBUG("pipe: reply not delivered (winerror=%lu)", GetLastError());
    }
    LeaveCriticalSection(&g_pipeWriteLock);
}

static void pipe_reply(const char* status, const char* what, const char* detail) {
    char line[512];
    if (detail && *detail) snprintf(line, sizeof(line), "%s %s %s", status, what, detail);
    else snprintf(line, sizeof(line), "%s %s", status, what);
    pipe_send(line);
}

static void handle_pipe_line(const char* line) {
    LOG_TRACE("pipe rx: %s", line);
    if (strncmp(line, "UI ", 3) == 0) {
        std::string error;
        if (g_ui.load(line + 3, error)) {
            LOG("ui: document accepted");
            pipe_reply("OK", "ui", nullptr);
        } else {
            LOG_WARN("ui: %s", error.c_str());
            pipe_reply("ERR", "ui", error.c_str());
        }
        return;
    }
    if (strncmp(line, "SET ", 4) == 0) {
        const char* name = line + 4;
        const char* space = strchr(name, ' ');
        float value = 0.0f;
        if (space && parse_float_c(space + 1, value)) {
            g_ui.set_number(std::string(name, space - name), value);
        } else {
            LOG_WARN("pipe: malformed SET: %.80s", line);
        }
        return;
    }
    if (strncmp(line, "TXT ", 4) == 0) {
        const char* name = line + 4;
        const char* space = strchr(name, ' ');
        if (space) g_ui.set_text(std::string(name, space - name), std::string(space + 1));
        else LOG_WARN("pipe: malformed TXT: %.80s", line);
        return;
    }
    if (strncmp(line, "POS ", 4) == 0) {
        // "POS <screen> <insetX> <insetY> [anchor]". Where a panel sits is a decision about one
        // mod's interface, so the mod makes it: it reads the key, saves the position in its own
        // settings, and says where the box goes. Everything inside the box is still the file's.
        char screen[64] = {0}, anchor[16] = {0};
        float insetX = 0.0f, insetY = 0.0f;
        int fields = sscanf_s(line + 4, "%63s %f %f %15s",
                              screen, (unsigned)sizeof(screen), &insetX, &insetY,
                              anchor, (unsigned)sizeof(anchor));
        if (fields >= 3) {
            if (!g_ui.set_placement(screen, insetX, insetY, fields >= 4 ? anchor : nullptr)) {
                LOG_WARN("pipe: POS unknown screen %s", screen);
                pipe_reply("ERR", "pos", screen);
            }
        } else {
            LOG_WARN("pipe: malformed POS: %.80s", line);
            pipe_reply("ERR", "pos", "malformed");
        }
        return;
    }
    if (strncmp(line, "RAW ", 4) == 0) {
        const char* screen = line + 4;
        const char* space = strchr(screen, ' ');
        if (!space) {
            LOG_WARN("pipe: malformed RAW: %.80s", line);
            pipe_reply("ERR", "raw", "malformed");
            return;
        }
        std::string error;
        if (!g_ui.set_raw(std::string(screen, space - screen), space + 1, error)) {
            LOG_WARN("ui: raw shapes rejected: %s", error.c_str());
            pipe_reply("ERR", "raw", error.c_str());
        }
        return;
    }

    // The widget messages below can arrive every frame, so like SET they are never answered;
    // a mistake is logged. The Python side has checked the name and the type before sending,
    // so reaching the log at all means the two sides disagree about the document.
    if (strncmp(line, "VAL ", 4) == 0) {
        const char* id = line + 4;
        const char* space = strchr(id, ' ');
        if (!space) { LOG_WARN("pipe: malformed VAL: %.80s", line); return; }
        std::string name(id, space - id);
        switch (g_ui.set_widget_value(name, space + 1)) {
            case oreoui::SetResult::Ok:
            case oreoui::SetResult::Busy:   // the player has hold of it; their hand wins
                break;
            case oreoui::SetResult::Unknown:
                LOG_WARN("pipe: VAL for unknown widget %s", name.c_str());
                break;
            case oreoui::SetResult::WrongType:
                LOG_WARN("pipe: VAL of the wrong kind for widget %s: %.80s", name.c_str(), space + 1);
                break;
        }
        return;
    }
    if (strncmp(line, "OPT ", 4) == 0) {
        const char* id = line + 4;
        const char* space = strchr(id, ' ');
        if (!space) { LOG_WARN("pipe: malformed OPT: %.80s", line); return; }
        std::string error;
        if (!g_ui.set_options(std::string(id, space - id), space + 1, error))
            LOG_WARN("pipe: OPT rejected: %s", error.c_str());
        return;
    }
    if (strncmp(line, "ENA ", 4) == 0) {
        char name[64] = {0}; int on = 1;
        if (sscanf_s(line + 4, "%63s %d", name, (unsigned)sizeof(name), &on) == 2) {
            if (!g_ui.set_enabled(name, on != 0)) LOG_WARN("pipe: ENA unknown widget %s", name);
        } else LOG_WARN("pipe: malformed ENA: %.80s", line);
        return;
    }
    if (strncmp(line, "PAD", 3) == 0 && (line[3] == ' ' || line[3] == 0)) {
        // A button held is the pad in use. Only buttons: the helper does not report the sticks,
        // so steering with a stick alone does not switch the device over.
        if (line[3] && line[4]) InterlockedExchange(&g_inputDevice, kDeviceGamepad);
        apply_pad(line[3] ? line + 4 : "");
        return;
    }

    if (strncmp(line, "VIS", 3) == 0) {
        // "VIS <element> <0|1>" - the mod controls which panel elements are drawn. An unknown
        // name is logged rather than silently ignored, so a mod-side typo is caught. Not
        // answered down the pipe: this one can arrive every frame.
        char name[64] = {0}; int on = 1;
        if (sscanf_s(line + 3, "%63s %d", name, (unsigned)sizeof(name), &on) >= 1) {
            if (!g_ui.set_visible(name, on != 0))
                LOG_WARN("pipe: VIS unknown name %s", name);
        } else LOG_WARN("malformed VIS message");
    } else LOG_WARN("pipe: unknown message: %.80s", line);
}

static DWORD WINAPI pipe_thread(LPVOID) {
    const char* name = "\\\\.\\pipe\\nms_overlay";
    if (!g_pipeWriteLockReady) {
        InitializeCriticalSection(&g_pipeWriteLock);
        g_pipeWriteLockReady = true;
    }
    for (;;) {
        // 64 KiB each way. The old 4 KiB was smaller than a UI document, so the mod's write
        // blocked part-way through until this thread got round to reading - inside the game
        // process, under the GIL, on a frame the player was looking at. A buffer bigger than
        // anything we send means the write returns immediately and that stall cannot happen.
        HANDLE pipe = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 64 * 1024, 64 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) { Sleep(500); continue; }
        LOG("pipe: server ready, waiting for the mod to connect...");
        BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) { CloseHandle(pipe); continue; }
        LOG("pipe: mod connected.");
        EnterCriticalSection(&g_pipeWriteLock);
        g_pipeHandle = pipe;
        LeaveCriticalSection(&g_pipeWriteLock);
        pipe_send("HELLO ui-format 2");
        // Nothing said yet on this connection, so the device the player last touched - if any -
        // goes out on the first pass below: a mod that connects late still knows it.
        LONG deviceSent = kDeviceNone;
        {
            // Whatever the player did while nobody was listening belonged to a mod that is gone.
            std::lock_guard<std::mutex> guard(g_outboxLock);
            g_outbox.clear();
        }

        char buf[16 * 1024];
        // What is left over from the last read, waiting for its newline.
        //
        // This used to be handled by passing the tail straight to handle_pipe_line as if it
        // were a whole message. A pipe is a byte stream: a line split across two reads was
        // therefore processed twice, both halves as garbage. It went unnoticed because the mod
        // sent one short line per frame and rarely straddled a read - a UI document is
        // kilobytes and straddles every time.
        std::string pending;
        const size_t kMaxPending = 1 << 20;   // a line this long is a broken sender, not a mod
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) break;  // disconnected
            // Drained, not sampled: one read per wake-up left the rest sitting in the buffer
            // until the next tick, which is how a buffer fills up in the first place.
            while (avail > 0) {
                DWORD rd = 0;
                if (!ReadFile(pipe, buf, sizeof(buf), &rd, nullptr) || rd == 0) break;
                InterlockedAdd64(&g_wireBytes, (LONG64)rd);
                avail = avail > rd ? avail - rd : 0;
                pending.append(buf, rd);
                size_t start = 0;
                for (;;) {
                    size_t nl = pending.find('\n', start);
                    if (nl == std::string::npos) break;
                    if (nl > start) {
                        std::string message = pending.substr(start, nl - start);
                        if (!message.empty() && message.back() == '\r') message.pop_back();
                        if (!message.empty()) {
                            InterlockedIncrement64(&g_wireMessages);
                            handle_pipe_line(message.c_str());
                        }
                    }
                    start = nl + 1;
                }
                pending.erase(0, start);
                if (pending.size() > kMaxPending) {
                    LOG_WARN("pipe: %zu bytes with no newline, dropping", pending.size());
                    pending.clear();
                }
            }
            std::vector<std::string> outgoing;
            {
                std::lock_guard<std::mutex> guard(g_outboxLock);
                outgoing.swap(g_outbox);
            }
            for (const std::string& event : outgoing) pipe_send(event.c_str());
            LONG device = InterlockedCompareExchange(&g_inputDevice, 0, 0);
            if (device != deviceSent) {
                pipe_send(device == kDeviceGamepad ? "INPUT gamepad" : "INPUT mouse_keyboard");
                deviceSent = device;
            }
            Sleep(2);  // low latency for the per-frame value stream
        }
        EnterCriticalSection(&g_pipeWriteLock);
        g_pipeHandle = INVALID_HANDLE_VALUE;
        LeaveCriticalSection(&g_pipeWriteLock);
        // Buttons held when the Runtime went away would otherwise stay held in ImGui.
        apply_pad("");
        DisconnectNamedPipe(pipe); CloseHandle(pipe);
        LOG("pipe: mod disconnected. Nothing to draw until it reconnects.");
    }
}

static DWORD WINAPI OverlayMain(LPVOID) {
    ovlog::reset();
    LOG("=== Project Oreo Runtime overlay loaded ===");
    LOG("PID=%lu", GetCurrentProcessId());
    LOG("logging mode=%s max_file_mb=%llu keep_previous=%d",
        ovlog::threshold() == ovlog::Level::Trace ? "trace" :
        (ovlog::threshold() == ovlog::Level::Debug ? "debug" : "normal"),
        ovlog::max_bytes() / (1024ULL * 1024ULL), (int)ovlog::keep_previous());
    CreateThread(nullptr, 0, pipe_thread, nullptr, 0, nullptr);
    if (!resolve_trampolines_and_setup()) LOG("ABORT: setup failed.");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        // Before any thread that could take it exists.
        InitializeCriticalSection(&g_inputLock);
        CreateThread(nullptr, 0, OverlayMain, nullptr, 0, nullptr);
    }
    return TRUE;
}
