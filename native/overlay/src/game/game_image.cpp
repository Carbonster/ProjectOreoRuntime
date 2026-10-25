#include "game/game_image.h"

#include <windows.h>

#include <charconv>

#include "log.h"

namespace game_image {

namespace {

uint8_t* image_base() { return (uint8_t*)GetModuleHandleW(nullptr); }

IMAGE_NT_HEADERS* nt_headers() {
    uint8_t* image = image_base();
    return (IMAGE_NT_HEADERS*)(image + ((IMAGE_DOS_HEADER*)image)->e_lfanew);
}

}  // namespace

bool contains(const void* address) {
    uintptr_t start = (uintptr_t)image_base();
    uintptr_t end = start + nt_headers()->OptionalHeader.SizeOfImage;
    return (uintptr_t)address >= start && (uintptr_t)address < end;
}

bool parse_signature(const char* text, std::vector<int>& out) {
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

void* find_unique(const char* signature, const char* what) {
    std::vector<int> pattern;
    if (!parse_signature(signature, pattern)) {
        LOG_ERROR("scan: signature for %s does not parse.", what);
        return nullptr;
    }
    uint8_t* image = image_base();
    IMAGE_NT_HEADERS* nt = nt_headers();

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
        LOG_ERROR("scan: %s matched %d times in the game's code, expected once.", what, count);
        return nullptr;
    }
    LOG("scan: %s at NMS.exe+0x%llX.", what, (unsigned long long)(found - image));
    return found;
}

}  // namespace game_image
