#include "ui/file_access.h"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <vector>

#include "log.h"

namespace file_access {

namespace {

std::wstring widen(const std::string& utf8) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, &out[0], n);
    return out;
}

std::string narrow(const std::wstring& wide) {
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring full_path(const std::wstring& path) {
    if (path.empty() || path.find(L'\0') != std::wstring::npos) return std::wstring();
    DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (n == 0) return std::wstring();
    std::wstring out(n, L'\0');
    n = GetFullPathNameW(path.c_str(), n, &out[0], nullptr);
    out.resize(n);
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    for (wchar_t& c : out)
        if (c == L'/') c = L'\\';
    return out;
}

std::wstring lowered(std::wstring text) {
    if (!text.empty()) CharLowerBuffW(&text[0], (DWORD)text.size());
    return text;
}

// Lowercase and canonical; Windows paths ignore case.
std::mutex g_lock;
std::vector<std::wstring> g_roots;

bool inside(const std::wstring& path) {
    std::wstring candidate = lowered(full_path(path));
    if (candidate.empty()) return false;
    std::lock_guard<std::mutex> guard(g_lock);
    for (const std::wstring& root : g_roots) {
        if (candidate == root) return true;
        if (candidate.size() > root.size() && candidate.compare(0, root.size(), root) == 0 &&
            candidate[root.size()] == L'\\')
            return true;
    }
    return false;
}

class Files final : public Rml::FileInterface {
public:
    Rml::FileHandle Open(const Rml::String& path) override {
        std::wstring wide = widen(path);
        if (wide.empty() || !inside(wide)) {
            LOG_WARN("files: refused %s: not inside a folder a mod registered.", path.c_str());
            return 0;
        }
        FILE* file = nullptr;
        if (_wfopen_s(&file, full_path(wide).c_str(), L"rb") != 0) return 0;
        return (Rml::FileHandle)file;
    }

    // RmlUi does not always check what Open returned; a refused file reads as empty.
    void Close(Rml::FileHandle file) override {
        if (file) fclose((FILE*)file);
    }

    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override {
        return file ? fread(buffer, 1, size, (FILE*)file) : 0;
    }

    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        return file && fseek((FILE*)file, offset, origin) == 0;
    }

    size_t Tell(Rml::FileHandle file) override {
        return file ? (size_t)ftell((FILE*)file) : 0;
    }
};

Files g_files;

}  // namespace

Rml::FileInterface* get() { return &g_files; }

void allow(const std::string& dir) {
    std::wstring root = lowered(full_path(widen(dir)));
    if (root.empty()) {
        LOG_WARN("files: cannot use %s as a folder.", dir.c_str());
        return;
    }
    std::lock_guard<std::mutex> guard(g_lock);
    for (const std::wstring& known : g_roots)
        if (known == root) return;
    g_roots.push_back(root);
    LOG("files: %s is readable.", dir.c_str());
}

std::string canonical(const std::string& path) { return narrow(full_path(widen(path))); }

bool allowed(const std::string& path) {
    std::wstring wide = widen(path);
    return !wide.empty() && inside(wide);
}

}  // namespace file_access
