#include "ipc/protocol.h"

#include <cstring>
#include <vector>

#include "input/pad_input.h"
#include "ipc/json_scalar.h"
#include "log.h"
#include "ui/ui_commands.h"

namespace protocol {

namespace {

using ui_commands::Command;
using ui_commands::ShowMode;
using ui_commands::Verb;

constexpr size_t kMaxId = 64;

bool is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Mods and values: what RmlUi accepts as a variable name, so a mod's name can be its model's.
bool is_name(const std::string& text) {
    if (text.empty() || text.size() > kMaxId || !is_letter(text[0])) return false;
    for (char c : text)
        if (!is_letter(c) && !is_digit(c) && c != '_') return false;
    return true;
}

// "oreo_" is the Runtime's own: its diagnostics panel is a data model too.
bool is_mod(const std::string& text) { return is_name(text) && text.rfind("oreo_", 0) != 0; }

// RmlUi keeps these words for its own expressions.
bool is_value_name(const std::string& text) {
    static const char* const kReserved[] = {"it", "it_index", "ev", "true", "false", "size",
                                            "literal"};
    if (!is_name(text)) return false;
    for (const char* word : kReserved)
        if (_stricmp(text.c_str(), word) == 0) return false;
    return true;
}

bool is_doc_id(const std::string& text) {
    if (text.empty() || text.size() > kMaxId) return false;
    for (char c : text)
        if (!is_letter(c) && !is_digit(c) && c != '_' && c != '-') return false;
    return true;
}

// `count` words split off the front; `rest` is what follows the last one, spaces and all.
bool split(const char* args, size_t count, std::vector<std::string>& words, std::string& rest) {
    const char* p = args;
    for (size_t i = 0; i < count; ++i) {
        const char* space = strchr(p, ' ');
        if (!space && i + 1 < count) return false;
        words.emplace_back(p, space ? (size_t)(space - p) : strlen(p));
        p = space ? space + 1 : p + strlen(p);
    }
    rest = p;
    return true;
}

std::string err(const char* verb, const std::string& detail) {
    LOG_WARN("pipe: %s rejected: %s", verb, detail.c_str());
    return std::string("ERR ") + verb + " " + detail;
}

std::string on_root(const char* args) {
    std::vector<std::string> w;
    std::string dir;
    if (!split(args, 1, w, dir) || !is_mod(w[0]) || dir.empty()) return err("root", "malformed");
    ui_commands::post({Verb::Root, w[0], "", "", dir});
    return {};
}

std::string on_load(const char* args) {
    std::vector<std::string> w;
    std::string path;
    if (!split(args, 2, w, path) || !is_mod(w[0]) || !is_doc_id(w[1]) || path.empty())
        return err("load", "malformed");
    ui_commands::post({Verb::Load, w[0], w[1], "", path});
    return {};
}

std::string on_show(const char* args) {
    std::vector<std::string> w;
    std::string mode;
    if (!split(args, 2, w, mode) || !is_mod(w[0]) || !is_doc_id(w[1]))
        return err("show", "malformed");
    Command command{Verb::Show, w[0], w[1]};
    if (mode == "hide") command.mode = ShowMode::Hide;
    else if (mode == "show") command.mode = ShowMode::Show;
    else if (mode == "modal") command.mode = ShowMode::Modal;
    else return err("show", "mode must be hide, show or modal");
    ui_commands::post(std::move(command));
    return {};
}

std::string on_close(const char* args) {
    std::vector<std::string> w;
    std::string rest;
    if (!split(args, 2, w, rest) || !rest.empty() || !is_mod(w[0]) || !is_doc_id(w[1]))
        return err("close", "malformed");
    ui_commands::post({Verb::Close, w[0], w[1]});
    return {};
}

std::string on_set(const char* args) {
    std::vector<std::string> w;
    std::string json;
    if (!split(args, 2, w, json) || !is_mod(w[0]) || !is_value_name(w[1]))
        return err("set", "malformed");
    Command command{Verb::Set, w[0], "", w[1]};
    if (!json_scalar::parse(json, command.value))
        return err("set", w[1] + " is not a JSON boolean, number or string");
    ui_commands::post(std::move(command));
    return {};
}

std::string on_pad(const char* args) {
    pad_input::set_held(args);
    return {};
}

struct Handler {
    const char* verb;
    std::string (*handle)(const char* args);
};

const Handler kHandlers[] = {
    {"ROOT", on_root}, {"LOAD", on_load}, {"SHOW", on_show},
    {"CLOSE", on_close}, {"SET", on_set}, {"PAD", on_pad},
};

}  // namespace

std::string handle(const char* line) {
    LOG_TRACE("pipe rx: %s", line);
    const char* space = strchr(line, ' ');
    size_t verbLength = space ? (size_t)(space - line) : strlen(line);
    const char* args = space ? space + 1 : "";
    for (const Handler& handler : kHandlers) {
        if (strlen(handler.verb) == verbLength && strncmp(handler.verb, line, verbLength) == 0)
            return handler.handle(args);
    }
    LOG_WARN("pipe: unknown message: %.80s", line);
    return {};
}

void disconnected() {
    pad_input::set_held("");
    ui_commands::post({Verb::Disconnected});
}

}  // namespace protocol
