// Bounded component logger shared by the overlay. Every setting arrives in an environment
// variable that Python sets before the DLL is loaded; nothing here reads a file.
//
// It used to glue FISHING_MOD_DIR to a hardcoded "\fishing_mod.ini" and read [Logging] out of
// it. That made a component shared by every mod into a component belonging to exactly one: a
// second mod would have had to name its own settings file fishing_mod.ini, and if it did not,
// the overlay silently read nothing at all - the path fell back to .\fishing_mod.ini, relative
// to the game's working directory, where no such file has ever existed.
//
// Whoever starts the Runtime already passes it the log directory and the log mode. Passing the
// three remaining numbers the same way costs nothing, and it takes the guessed filename out of
// the binary entirely.
//
// Without a log directory the overlay writes NO log at all. It used to fall back to
// %TEMP%\FishingMod_logs, which meant a folder nobody was told about, nobody cleaned up, and
// nobody thought to look in - the logs were being written the whole time, just not where anyone
// expected. Refusing to write is the honest answer: whoever starts the Runtime decides where
// its logs go, and if they did not say, there is no good guess to make.
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ovlog {

enum class Level { Trace = 0, Debug = 1, Info = 2, Warning = 3, Error = 4 };

inline std::string env(const char* name) {
    char value[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableA(name, value, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? std::string(value) : std::string();
}

// Out of range is clamped and unparsable falls back to the default, rather than either one
// refusing to log. A logger that switches itself off because a number was mistyped helps nobody.
inline int env_int(const char* name, int fallback, int low, int high) {
    std::string raw = env(name);
    if (raw.empty()) return fallback;
    char* end = nullptr;
    long parsed = strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str() || *end != 0) return fallback;
    if (parsed < low) return low;
    if (parsed > high) return high;
    return (int)parsed;
}

inline bool env_bool(const char* name, bool fallback) {
    std::string raw = env(name);
    if (raw.empty()) return fallback;
    return _stricmp(raw.c_str(), "false") != 0 && _stricmp(raw.c_str(), "0") != 0;
}

inline std::string log_dir() { return env("FISHING_MOD_LOG_DIR"); }

// No directory means logging is off. Checked before every write, so an empty path can never
// turn into a file at the root of a drive.
inline bool logging_enabled() { return !log_dir().empty(); }

inline std::string log_path() { return log_dir() + "\\overlay.log"; }
inline std::string previous_dir() { return log_dir() + "\\previous"; }
inline std::string previous_path() { return previous_dir() + "\\overlay.log"; }
inline Level& threshold() { static Level value = Level::Info; return value; }
inline ULONGLONG& trace_deadline() { static ULONGLONG value = 0; return value; }
inline ULONGLONG& max_bytes() { static ULONGLONG value = 10ULL * 1024 * 1024; return value; }
inline bool& keep_previous() { static bool value = true; return value; }
inline bool& configured() { static bool value = false; return value; }

inline void load_settings() {
    if (configured()) return;
    configured() = true;
    std::string mode = env("FISHING_MOD_LOG_MODE");
    if (_stricmp(mode.c_str(), "trace") == 0) threshold() = Level::Trace;
    else if (_stricmp(mode.c_str(), "debug") == 0) threshold() = Level::Debug;
    else threshold() = Level::Info;
    max_bytes() = (ULONGLONG)env_int("FISHING_MOD_LOG_MAX_MB", 10, 1, 50) * 1024 * 1024;
    keep_previous() = env_bool("FISHING_MOD_LOG_KEEP_PREVIOUS", true);
    int minutes = env_int("FISHING_MOD_LOG_TRACE_MINUTES", 10, 1, 60);
    trace_deadline() = threshold() == Level::Trace ? GetTickCount64() + (ULONGLONG)minutes * 60000 : 0;
    if (!logging_enabled()) return;
    CreateDirectoryA(log_dir().c_str(), nullptr);
    CreateDirectoryA(previous_dir().c_str(), nullptr);
}

inline void rotate_current() {
    if (keep_previous())
        MoveFileExA(log_path().c_str(), previous_path().c_str(), MOVEFILE_REPLACE_EXISTING);
    else
        DeleteFileA(log_path().c_str());
}

inline void reset() {
    load_settings();
    if (!logging_enabled()) return;
    rotate_current();
    FILE* f = nullptr; fopen_s(&f, log_path().c_str(), "w"); if (f) fclose(f);
}

inline const char* level_name(Level level) {
    switch (level) {
        case Level::Trace: return "TRACE"; case Level::Debug: return "DEBUG";
        case Level::Warning: return "WARNING"; case Level::Error: return "ERROR";
        default: return "INFO";
    }
}

inline bool enabled(Level level) {
    load_settings();
    if (!logging_enabled()) return false;
    Level effective = threshold();
    if (effective == Level::Trace && GetTickCount64() > trace_deadline()) effective = Level::Debug;
    return (int)level >= (int)effective;
}

inline void rotate_if_full() {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(log_path().c_str(), GetFileExInfoStandard, &data)) return;
    ULONGLONG size = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    if (size >= max_bytes()) rotate_current();
}

inline void redact_value(std::string& text, const char* value, const char* replacement) {
    if (!value || !*value) return;
    size_t valueLen = strlen(value);
    for (size_t pos = 0; pos + valueLen <= text.size();) {
        if (_strnicmp(text.c_str() + pos, value, valueLen) == 0) {
            text.replace(pos, valueLen, replacement); pos += strlen(replacement);
        } else ++pos;
    }
}

inline std::string redact(std::string text) {
    const char* vars[] = {"FISHING_MOD_DIR", "LOCALAPPDATA", "APPDATA", "USERPROFILE", "TEMP", "TMP"};
    const char* tags[] = {"<MOD_DIR>", "<LOCALAPPDATA>", "<APPDATA>", "<USERPROFILE>", "<TEMP>", "<TEMP>"};
    char value[MAX_PATH] = {0};
    for (int i = 0; i < 6; ++i) {
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

inline void write_v(Level level, const char* fmt, va_list args) {
    if (!enabled(level)) return;
    static CRITICAL_SECTION cs;
    static bool inited = false;
    if (!inited) { InitializeCriticalSection(&cs); inited = true; }
    EnterCriticalSection(&cs);
    rotate_if_full();
    FILE* f = nullptr; fopen_s(&f, log_path().c_str(), "a");
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%-7s] [%lu:%lu] [OVERLAY] ",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            level_name(level), GetCurrentProcessId(), GetCurrentThreadId());
        char message[2048] = {0};
        vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);
        std::string safe = redact(message);
        fprintf(f, "%s\n", safe.c_str()); fclose(f);
    }
    LeaveCriticalSection(&cs);
}

inline void write(Level level, const char* fmt, ...) {
    va_list args; va_start(args, fmt); write_v(level, fmt, args); va_end(args);
}

inline void write_auto(const char* fmt, ...) {
    Level level = Level::Info;
    if (strncmp(fmt, "ERROR", 5) == 0 || strncmp(fmt, "ABORT", 5) == 0) level = Level::Error;
    else if (strncmp(fmt, "WARN", 4) == 0) level = Level::Warning;
    va_list args; va_start(args, fmt); write_v(level, fmt, args); va_end(args);
}

}  // namespace ovlog

#define LOG(...)       ovlog::write_auto(__VA_ARGS__)
#define LOG_DEBUG(...) ovlog::write(ovlog::Level::Debug, __VA_ARGS__)
#define LOG_TRACE(...) ovlog::write(ovlog::Level::Trace, __VA_ARGS__)
#define LOG_WARN(...)  ovlog::write(ovlog::Level::Warning, __VA_ARGS__)
#define LOG_ERROR(...) ovlog::write(ovlog::Level::Error, __VA_ARGS__)
