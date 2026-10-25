#include "ui/rml_system.h"

#include <windows.h>

#include <string>
#include <unordered_set>
#include <vector>


#include "ipc/outbox.h"
#include "log.h"

namespace rml_system {

namespace {

constexpr size_t kMaxRemembered = 256;

std::wstring widen(const std::string& utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &out[0], n);
    return out;
}

std::string narrow(const wchar_t* text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], n, nullptr, nullptr);
    return out;
}

class System final : public Rml::SystemInterface {
public:
    double GetElapsedTime() override {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return (double)(now.QuadPart - start_.QuadPart) / (double)frequency_.QuadPart;
    }

    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        bool problem = type == Rml::Log::LT_ERROR || type == Rml::Log::LT_WARNING ||
                       type == Rml::Log::LT_ASSERT;
        if (!problem) {
            LOG_DEBUG("rmlui: %s", message.c_str());
            return true;
        }
        if (said_.count(message)) return true;
        if (said_.size() < kMaxRemembered) said_.insert(message);
        bool error = type != Rml::Log::LT_WARNING;
        if (error) LOG_ERROR("rmlui: %s", message.c_str());
        else LOG_WARN("rmlui: %s", message.c_str());
        std::vector<std::string> line = {std::string("LOG ") + (error ? "error " : "warning ") +
                                         one_line(message)};
        outbox::post(line);
        return true;   // never break into a debugger inside somebody's game
    }

    void SetClipboardText(const Rml::String& text) override {
        if (!OpenClipboard(nullptr)) return;
        EmptyClipboard();
        std::wstring wide = widen(text);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
        if (memory) {
            memcpy(GlobalLock(memory), wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        }
        CloseClipboard();
    }

    void GetClipboardText(Rml::String& text) override {
        text.clear();
        if (!OpenClipboard(nullptr)) return;
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
            if (const wchar_t* wide = (const wchar_t*)GlobalLock(data)) {
                text = narrow(wide);
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
    }

    void forget() { said_.clear(); }

private:
    LARGE_INTEGER frequency_ = query(true);
    LARGE_INTEGER start_ = query(false);
    std::unordered_set<std::string> said_;

    static LARGE_INTEGER query(bool frequency) {
        LARGE_INTEGER value;
        if (frequency) QueryPerformanceFrequency(&value);
        else QueryPerformanceCounter(&value);
        return value;
    }

    static std::string one_line(std::string text) {
        for (char& c : text)
            if (c == '\n' || c == '\r') c = ' ';
        return text;
    }
};

System g_system;

}  // namespace

Rml::SystemInterface* get() { return &g_system; }

void forget_repeats() { g_system.forget(); }

}  // namespace rml_system
