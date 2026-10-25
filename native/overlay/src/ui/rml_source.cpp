#include "ui/rml_source.h"

#include <cctype>

namespace rml_source {

namespace {

// Where the tag `<name ...>` ends, or npos. Case-insensitive; `<header>` is not `<head>`.
size_t end_of_tag(const std::string& text, const char* name) {
    std::string lower(text.size(), '\0');
    for (size_t i = 0; i < text.size(); ++i) lower[i] = (char)std::tolower((unsigned char)text[i]);
    std::string open = std::string("<") + name;
    for (size_t at = lower.find(open); at != std::string::npos; at = lower.find(open, at + 1)) {
        char next = at + open.size() < lower.size() ? lower[at + open.size()] : '\0';
        if (next != '>' && next != '/' && !std::isspace((unsigned char)next)) continue;
        size_t close = lower.find('>', at);
        return close == std::string::npos ? close : close + 1;
    }
    return std::string::npos;
}

}  // namespace

bool link_base_sheet(std::string& rml, const std::string& sheetPath) {
    std::string link = "<link type=\"text/rcss\" href=\"" + sheetPath + "\"/>";
    size_t head = end_of_tag(rml, "head");
    if (head != std::string::npos) {
        if (rml[head - 2] == '/') {   // <head/>: open it up
            rml.replace(head - 2, 2, ">" + link + "</head>");
            return true;
        }
        rml.insert(head, link);
        return true;
    }
    size_t root = end_of_tag(rml, "rml");
    if (root == std::string::npos) return false;
    rml.insert(root, "<head>" + link + "</head>");
    return true;
}

}  // namespace rml_source
