// External SDL controller component for Project Oreo Runtime.
// Pipe client: \\.\pipe\nms_controller. Sends
//   BTN a,x,up
// whenever the pressed set changes, plus a keepalive a few times a second so a lost release
// can't leave the mod holding a stale press. Reconnects if the mod isn't up yet / restarts.
//
// Buttons are read from two sources and merged (union): SDL, and - for XInput-class pads (Xbox
// and the many pads that emulate it) - XInput directly. This exists because SDL2 on Windows reads
// an XInput-class pad's face buttons from raw HID reports but patches its triggers in from XInput;
// on at least one Xbox pad the HID reports never arrived, so SDL saw the triggers but never A/B/X/Y
// while XInput saw everything (ported from the fishing mod's controller_support fix; see its
// CHANGELOG.md for the diagnostic log that confirmed this). DualSense and other pads with no
// XInput slot are unaffected: XInput contributes nothing for them and SDL's report is used as-is.

#include <windows.h>
#include <tlhelp32.h>        // process enumeration, to auto-exit when the game closes
#include <xinput.h>          // types/constants only; the DLL is loaded dynamically below
#define SDL_MAIN_HANDLED     // plain console main(), don't let SDL rename it / require SDL2main
#include <SDL.h>
#include <string>
#include <vector>
#include <set>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>

static const char* PIPE_NAME = "\\\\.\\pipe\\nms_controller";
static const char* GAME_PROC = "NMS.exe";
static const char* INSTANCE_MUTEX = "Local\\ProjectOreoControllerSupport";
static const DWORD GAME_START_TIMEOUT_MS = 30000;

enum class LogLevel { Trace = 0, Debug = 1, Info = 2, Warning = 3, Error = 4 };
static std::string g_logPath, g_previousPath;
static LogLevel g_logThreshold = LogLevel::Info;
static ULONGLONG g_logMaxBytes = 10ULL * 1024 * 1024;
static ULONGLONG g_traceDeadline = 0;
static bool g_keepPrevious = true;

static const char* log_level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE"; case LogLevel::Debug: return "DEBUG";
        case LogLevel::Warning: return "WARNING"; case LogLevel::Error: return "ERROR";
        default: return "INFO";
    }
}

static void rotate_log() {
    if (g_logPath.empty()) return;   // logging is off; there is nothing to rotate
    if (g_keepPrevious)
        MoveFileExA(g_logPath.c_str(), g_previousPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    else
        DeleteFileA(g_logPath.c_str());
}

static bool log_enabled(LogLevel level) {
    LogLevel effective = g_logThreshold;
    if (effective == LogLevel::Trace && GetTickCount64() > g_traceDeadline) effective = LogLevel::Debug;
    return (int)level >= (int)effective;
}

static void redact_value(std::string& text, const char* value, const char* replacement) {
    if (!value || !*value) return;
    size_t valueLen = strlen(value);
    for (size_t pos = 0; pos + valueLen <= text.size();) {
        if (_strnicmp(text.c_str() + pos, value, valueLen) == 0) {
            text.replace(pos, valueLen, replacement); pos += strlen(replacement);
        } else ++pos;
    }
}

static std::string redact(std::string text) {
    const char* vars[] = {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "TEMP", "TMP"};
    const char* tags[] = {"<LOCALAPPDATA>", "<APPDATA>", "<USERPROFILE>", "<TEMP>", "<TEMP>"};
    char value[MAX_PATH] = {0};
    for (int i = 0; i < 5; ++i) {
        DWORD n = GetEnvironmentVariableA(vars[i], value, MAX_PATH);
        if (n > 0 && n < MAX_PATH) redact_value(text, value, tags[i]);
    }
    char identity[256] = {0};
    DWORD n = GetEnvironmentVariableA("USERNAME", identity, sizeof(identity));
    if (n >= 4 && n < sizeof(identity)) redact_value(text, identity, "<USER>");
    n = GetEnvironmentVariableA("COMPUTERNAME", identity, sizeof(identity));
    if (n >= 4 && n < sizeof(identity)) redact_value(text, identity, "<HOST>");
    return text;
}

static void log_message(LogLevel level, const char* fmt, ...) {
    if (!log_enabled(level)) return;
    if (g_logPath.empty()) return;   // no directory was given, so nothing is written anywhere
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExA(g_logPath.c_str(), GetFileExInfoStandard, &data)) {
        ULONGLONG size = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        if (size >= g_logMaxBytes) rotate_log();
    }
    FILE* f = nullptr; fopen_s(&f, g_logPath.c_str(), "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%-7s] [%lu:%lu] [CONTROLLER] ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        log_level_name(level), GetCurrentProcessId(), GetCurrentThreadId());
    char message[2048] = {0};
    va_list args; va_start(args, fmt);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);
    va_end(args);
    std::string safe = redact(message);
    fprintf(f, "%s\n", safe.c_str()); fclose(f);
}

// Settings arrive in the environment. Out of range is clamped and unparsable falls back to the
// default: a helper that logs nothing because a number was mistyped is worse than one that logs
// at its default.
static int env_int(const char* name, int fallback, int low, int high) {
    char raw[32] = {0};
    DWORD n = GetEnvironmentVariableA(name, raw, sizeof(raw));
    if (n == 0 || n >= sizeof(raw)) return fallback;
    char* end = nullptr;
    long parsed = strtol(raw, &end, 10);
    if (end == raw || *end != 0) return fallback;
    if (parsed < low) return low;
    if (parsed > high) return high;
    return (int)parsed;
}

static bool env_bool(const char* name, bool fallback) {
    char raw[16] = {0};
    DWORD n = GetEnvironmentVariableA(name, raw, sizeof(raw));
    if (n == 0 || n >= sizeof(raw)) return fallback;
    return _stricmp(raw, "false") != 0 && _stricmp(raw, "0") != 0;
}

static void init_logging() {
    char selfPath[MAX_PATH] = {0}; GetModuleFileNameA(NULL, selfPath, MAX_PATH);
    std::string exePath(selfPath);
    size_t slash = exePath.find_last_of("\\/");
    std::string exeDir = slash == std::string::npos ? std::string(".") : exePath.substr(0, slash);
    std::string modDir = exeDir;
    char configuredModDir[MAX_PATH] = {0};
    DWORD configuredModDirLength =
        GetEnvironmentVariableA("FISHING_MOD_DIR", configuredModDir, MAX_PATH);
    if (configuredModDirLength > 0 && configuredModDirLength < MAX_PATH)
        modDir = configuredModDir;
    size_t parentSlash = exeDir.find_last_of("\\/");
    std::string leaf = parentSlash == std::string::npos ? exeDir : exeDir.substr(parentSlash + 1);
    if (configuredModDirLength == 0 && _stricmp(leaf.c_str(), "bin") == 0)
        modDir = parentSlash == std::string::npos ? std::string(".") : exeDir.substr(0, parentSlash);
    // Nobody normally tells this process where to log. Explorer starts it - the only way to
    // stay out of Steam's game process tree - and Explorer passes neither arguments nor
    // environment, so the Runtime cannot hand the mod's log directory over until the pipe
    // becomes bidirectional (v2). Until then, log into a "logs" folder in the Runtime package
    // next to the exe. pip only removes files it installed, so this folder survives Runtime
    // updates and is left behind on uninstall: a few kilobytes, deliberately accepted, and far
    // better than a controller bug with no log at all.
    std::string logDir = modDir + "\\logs";
    char configuredLogDir[MAX_PATH] = {0};
    DWORD configuredLogDirLength =
        GetEnvironmentVariableA("FISHING_MOD_LOG_DIR", configuredLogDir, MAX_PATH);
    if (configuredLogDirLength > 0 && configuredLogDirLength < MAX_PATH)
        logDir = configuredLogDir;   // only reached when the exe is started by hand for debugging
    std::string previousDir = logDir + "\\previous";
    CreateDirectoryA(logDir.c_str(), nullptr); CreateDirectoryA(previousDir.c_str(), nullptr);
    g_logPath = logDir + "\\controller.log"; g_previousPath = previousDir + "\\controller.log";
    // Logging settings come from the environment, the same three variables the overlay reads.
    // They used to be read out of modDir + a hardcoded fishing_mod.ini - one mod's filename
    // compiled into a component meant to serve any mod, and one that in practice was never
    // found: Explorer passes no environment, so FISHING_MOD_DIR is empty here, modDir falls
    // back to the Runtime package, and no fishing_mod.ini has ever lived there. These settings
    // have been silently sitting at their defaults the whole time.
    //
    // They still will, for the same reason - reading the environment does not conjure one that
    // was never passed. What changes is that the helper no longer names one mod's file, and the
    // day the helper's own pipe learns to talk back (v2) these are the values that come down it.
    char mode[32] = {0};
    DWORD modeLength = GetEnvironmentVariableA("FISHING_MOD_LOG_MODE", mode, sizeof(mode));
    if (modeLength == 0 || modeLength >= sizeof(mode)) strcpy_s(mode, "normal");
    if (_stricmp(mode, "trace") == 0) g_logThreshold = LogLevel::Trace;
    else if (_stricmp(mode, "debug") == 0) g_logThreshold = LogLevel::Debug;
    int mb = env_int("FISHING_MOD_LOG_MAX_MB", 10, 1, 50);
    g_logMaxBytes = (ULONGLONG)mb * 1024 * 1024;
    g_keepPrevious = env_bool("FISHING_MOD_LOG_KEEP_PREVIOUS", true);
    int minutes = env_int("FISHING_MOD_LOG_TRACE_MINUTES", 10, 1, 60);
    g_traceDeadline = g_logThreshold == LogLevel::Trace ? GetTickCount64() + (ULONGLONG)minutes * 60000 : 0;
    rotate_log();
    FILE* f = nullptr; fopen_s(&f, g_logPath.c_str(), "w"); if (f) fclose(f);
    log_message(LogLevel::Info, "logging ready: mode=%s max_file_mb=%d keep_previous=%d",
        mode, mb, (int)g_keepPrevious);
}

// Used to stop the helper after the game closes.
static bool process_running(const char* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 pe; pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0) { found = true; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static DWORD parse_game_pid(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], "--game-pid") == 0) {
            char* end = nullptr;
            unsigned long value = strtoul(argv[i + 1], &end, 10);
            if (end && *end == '\0' && value > 0) return static_cast<DWORD>(value);
        }
    }
    return 0;
}

// SDL GameController button -> short name (same names the mod expects)
struct BtnName { SDL_GameControllerButton btn; const char* name; };
static const BtnName BUTTONS[] = {
    { SDL_CONTROLLER_BUTTON_A, "a" },
    { SDL_CONTROLLER_BUTTON_B, "b" },
    { SDL_CONTROLLER_BUTTON_X, "x" },
    { SDL_CONTROLLER_BUTTON_Y, "y" },
    { SDL_CONTROLLER_BUTTON_DPAD_UP, "up" },
    { SDL_CONTROLLER_BUTTON_DPAD_DOWN, "down" },
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, "left" },
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, "right" },
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "lb" },
    { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "rb" },
    { SDL_CONTROLLER_BUTTON_BACK, "back" },
    { SDL_CONTROLLER_BUTTON_START, "start" },
    { SDL_CONTROLLER_BUTTON_LEFTSTICK, "ls" },
    { SDL_CONTROLLER_BUTTON_RIGHTSTICK, "rs" },
};

// Canonical button order shared by every source, so the merged output is stable regardless of
// which source(s) contributed a given name.
static const char* BTN_ORDER[] = {
    "a", "b", "x", "y", "up", "down", "left", "right",
    "lb", "rb", "back", "start", "ls", "rs", "lt", "rt",
};

static void collect_sdl_names(SDL_GameController* c, std::set<std::string>& out) {
    for (const auto& b : BUTTONS) {
        if (SDL_GameControllerGetButton(c, b.btn)) out.insert(b.name);
    }
    // triggers as buttons (analog -> pressed past halfway; axis range 0..32767)
    if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) out.insert("lt");
    if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) out.insert("rt");
}

static std::string names_to_line(const std::set<std::string>& names, const char* prefix) {
    std::string s = prefix;
    bool first = true;
    for (const char* n : BTN_ORDER) {
        if (names.count(n)) {
            if (!first) s += ",";
            s += n; first = false;
        }
    }
    return s;
}

// ---- XInput reader -------------------------------------------------------------------------
// Loaded dynamically so the helper still starts on a machine without a given xinput DLL, and so
// the build needs no extra link libraries.
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XINPUT_STATE*);

static HMODULE g_xinputDll = nullptr;
static XInputGetState_t g_xinputGetState = nullptr;
static const char* g_xinputDllName = "(none)";

static void xinput_load() {
    // Newest first; 9_1_0 ships with every supported Windows and is the last-resort fallback.
    static const char* candidates[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (const char* name : candidates) {
        HMODULE dll = LoadLibraryA(name);
        if (!dll) continue;
        auto fn = (XInputGetState_t)GetProcAddress(dll, "XInputGetState");
        if (fn) {
            g_xinputDll = dll; g_xinputGetState = fn; g_xinputDllName = name;
            log_message(LogLevel::Info, "xinput: using %s", name);
            return;
        }
        FreeLibrary(dll);
    }
    log_message(LogLevel::Warning, "xinput: no usable XInput DLL found");
}

static void collect_xinput_names(const XINPUT_GAMEPAD& pad, std::set<std::string>& out) {
    struct XBtn { WORD mask; const char* name; };
    static const XBtn XBUTTONS[] = {
        { XINPUT_GAMEPAD_A, "a" }, { XINPUT_GAMEPAD_B, "b" },
        { XINPUT_GAMEPAD_X, "x" }, { XINPUT_GAMEPAD_Y, "y" },
        { XINPUT_GAMEPAD_DPAD_UP, "up" }, { XINPUT_GAMEPAD_DPAD_DOWN, "down" },
        { XINPUT_GAMEPAD_DPAD_LEFT, "left" }, { XINPUT_GAMEPAD_DPAD_RIGHT, "right" },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, "lb" }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, "rb" },
        { XINPUT_GAMEPAD_BACK, "back" }, { XINPUT_GAMEPAD_START, "start" },
        { XINPUT_GAMEPAD_LEFT_THUMB, "ls" }, { XINPUT_GAMEPAD_RIGHT_THUMB, "rs" },
    };
    for (const auto& b : XBUTTONS) {
        if (pad.wButtons & b.mask) out.insert(b.name);
    }
    // 125/255 matches the 16000/32767 halfway point used for the SDL trigger axes above.
    if (pad.bLeftTrigger > 125) out.insert("lt");
    if (pad.bRightTrigger > 125) out.insert("rt");
}

// Reads the active slot, rescanning for one at most once a second (polling four empty slots every
// ~10ms is not free). Returns false when no slot is connected - normal for a non-XInput pad such
// as DualSense, which is then read through SDL alone.
static bool xinput_poll(XINPUT_GAMEPAD& pad, int& slot) {
    static int active = -1;
    static DWORD lastScan = 0;
    if (!g_xinputGetState) return false;

    XINPUT_STATE state{};
    if (active >= 0) {
        if (g_xinputGetState((DWORD)active, &state) == ERROR_SUCCESS) {
            pad = state.Gamepad; slot = active;
            return true;
        }
        log_message(LogLevel::Info, "xinput: slot %d disconnected", active);
        active = -1;
    }
    DWORD now = GetTickCount();
    if (now - lastScan < 1000) return false;
    lastScan = now;
    for (int i = 0; i < 4; ++i) {
        if (g_xinputGetState((DWORD)i, &state) != ERROR_SUCCESS) continue;
        active = i;
        log_message(LogLevel::Info, "xinput: slot %d connected (%s)", i, g_xinputDllName);
        pad = state.Gamepad; slot = i;
        return true;
    }
    return false;
}

static HANDLE connect_pipe() {
    HANDLE h = CreateFileA(PIPE_NAME, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    return h;
}

static bool pipe_send(HANDLE h, const std::string& line) {
    std::string data = line + "\n";
    DWORD written = 0;
    return WriteFile(h, data.c_str(), (DWORD)data.size(), &written, nullptr) != 0;
}

int main(int argc, char** argv) {
    HANDLE instanceMutex = CreateMutexA(nullptr, FALSE, INSTANCE_MUTEX);
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instanceMutex);
        return 0;
    }

    init_logging();
    DWORD gamePid = parse_game_pid(argc, argv);
    HANDLE gameProcess = nullptr;
    if (gamePid != 0) {
        gameProcess = OpenProcess(SYNCHRONIZE, FALSE, gamePid);
        if (!gameProcess) {
            log_message(LogLevel::Error, "cannot watch game PID %lu (winerror=%lu)",
                gamePid, GetLastError());
            if (instanceMutex) CloseHandle(instanceMutex);
            return 1;
        }
    }
    // Record whether Steam Input is active for this process.
    char steamAppId[64] = {};
    DWORD steamAppIdLength = GetEnvironmentVariableA(
        "SteamAppId", steamAppId, static_cast<DWORD>(sizeof(steamAppId)));
    const char* steamAppIdText =
        steamAppIdLength > 0 && steamAppIdLength < sizeof(steamAppId) ? steamAppId : "(unset)";
    log_message(LogLevel::Info, "startup: game_pid=%lu SteamAppId=%s SDL=%d.%d.%d",
        gamePid, steamAppIdText, SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
    xinput_load();
    SDL_SetMainReady();
    // Let SDL read controllers without needing a window; enable background events so it keeps
    // reporting even when the game window (not us) has focus.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
        log_message(LogLevel::Error, "SDL_Init failed: %s", SDL_GetError());
        if (instanceMutex) CloseHandle(instanceMutex);
        return 1;
    }
    log_message(LogLevel::Info, "waiting for a controller and the mod pipe");

    SDL_GameController* ctrl = nullptr;
    HANDLE pipe = nullptr;
    std::string last_line;
    DWORD last_keepalive = 0;
    bool game_seen = gameProcess != nullptr;
    DWORD game_wait_started = GetTickCount();
    DWORD last_proc_check = 0;
    DWORD last_nojoy_log = 0;

    for (;;) {
        // Auto-exit when the game closes. Watch NMS.exe: once we've seen it running, exit the
        // moment it's gone. Before the first sighting, wait for a bounded startup period so a
        // failed launch cannot leave this process behind. This is independent of controller
        // discovery: a controller may be connected at any time while the game is running.
        // Throttled to about 1 Hz because the Toolhelp snapshot is not free.
        DWORD now_pc = GetTickCount();
        if (now_pc - last_proc_check > 1000) {
            last_proc_check = now_pc;
            bool gameRunning = gameProcess
                ? WaitForSingleObject(gameProcess, 0) == WAIT_TIMEOUT
                : process_running(GAME_PROC);
            if (gameRunning) {
                game_seen = true;
            } else if (game_seen) {
                log_message(LogLevel::Info, "game closed - exiting");
                break;
            } else if (now_pc - game_wait_started >= GAME_START_TIMEOUT_MS) {
                log_message(LogLevel::Warning, "game (%s) was not detected within %lu seconds - exiting",
                    GAME_PROC, GAME_START_TIMEOUT_MS / 1000);
                break;
            }
        }

        SDL_PumpEvents();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            // SDL reports both existing and newly connected controllers here.
            if (ev.type == SDL_CONTROLLERDEVICEADDED && !ctrl) {
                int idx = ev.cdevice.which;   // device index (ADDED); pass to SDL_GameControllerOpen
                if (SDL_IsGameController(idx)) {
                    ctrl = SDL_GameControllerOpen(idx);
                    if (ctrl) {
                        log_message(LogLevel::Info, "controller connected: type=%d vendor=0x%04X product=0x%04X",
                            (int)SDL_GameControllerGetType(ctrl), SDL_GameControllerGetVendor(ctrl),
                            SDL_GameControllerGetProduct(ctrl));
                        last_line.clear();
                    }
                }
            } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED && ctrl) {
                SDL_GameControllerClose(ctrl); ctrl = nullptr;
                log_message(LogLevel::Warning, "controller disconnected");
            }
        }

        // (re)acquire a controller - fallback scan in case we missed the ADDED event.
        if (!ctrl) {
            // Log controller visibility at most once per second.
            if (now_pc - last_nojoy_log > 1000) {
                last_nojoy_log = now_pc;
                log_message(LogLevel::Debug, "no controller yet (SDL_NumJoysticks=%d)", SDL_NumJoysticks());
            }
            for (int i = 0; i < SDL_NumJoysticks(); ++i) {
                if (SDL_IsGameController(i)) {
                    ctrl = SDL_GameControllerOpen(i);
                    if (ctrl) {
                        log_message(LogLevel::Info, "controller connected: type=%d vendor=0x%04X product=0x%04X",
                            (int)SDL_GameControllerGetType(ctrl), SDL_GameControllerGetVendor(ctrl),
                            SDL_GameControllerGetProduct(ctrl));
                        last_line.clear();
                        break;
                    }
                }
            }
        }

        // (re)connect the pipe to the mod
        if (!pipe) {
            pipe = connect_pipe();
            if (pipe) { log_message(LogLevel::Info, "connected to mod pipe"); last_line.clear(); }
        }

        // Union of what SDL reports and what XInput reports. For a non-XInput pad (e.g. DualSense)
        // xinput_poll() finds no slot and contributes nothing, so this is just the SDL view.
        XINPUT_GAMEPAD xi_pad{};
        int xi_slot = -1;
        bool xi_ok = xinput_poll(xi_pad, xi_slot);
        if (ctrl || xi_ok) {
            std::set<std::string> names;
            if (ctrl) collect_sdl_names(ctrl, names);
            if (xi_ok) collect_xinput_names(xi_pad, names);
            std::string line = names_to_line(names, "BTN ");

            DWORD now = GetTickCount();
            if (line != last_line) {
                // Log state changes so controller.log shows whether input is being read at all.
                log_message(LogLevel::Debug, "buttons -> [%s]", line.c_str() + 4);
            }
            if (pipe && (line != last_line || (now - last_keepalive) > 200)) {
                if (!pipe_send(pipe, line)) {
                    log_message(LogLevel::Warning, "mod pipe lost; will reconnect");
                    CloseHandle(pipe); pipe = nullptr;
                } else {
                    last_line = line;
                    last_keepalive = now;
                }
            }
        }

        Sleep(10);  // ~100 Hz
    }

    // Clean shutdown (reached when the game closed) - release the controller, pipe, and log
    // handle so nothing stays locked after the game quits.
    if (ctrl) SDL_GameControllerClose(ctrl);
    if (pipe) CloseHandle(pipe);
    if (g_xinputDll) { FreeLibrary(g_xinputDll); g_xinputDll = nullptr; g_xinputGetState = nullptr; }
    SDL_Quit();
    if (gameProcess) CloseHandle(gameProcess);
    if (instanceMutex) CloseHandle(instanceMutex);
    return 0;
}
