#include "ipc/json_scalar.h"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace json_scalar {

namespace {

bool hex4(const char*& p, const char* end, unsigned& out) {
    if (end - p < 4) return false;
    auto result = std::from_chars(p, p + 4, out, 16);
    if (result.ptr != p + 4) return false;
    p += 4;
    return true;
}

void append_utf8(std::string& out, unsigned code) {
    if (code < 0x80) {
        out += (char)code;
    } else if (code < 0x800) {
        out += (char)(0xC0 | (code >> 6));
        out += (char)(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += (char)(0xE0 | (code >> 12));
        out += (char)(0x80 | ((code >> 6) & 0x3F));
        out += (char)(0x80 | (code & 0x3F));
    } else {
        out += (char)(0xF0 | (code >> 18));
        out += (char)(0x80 | ((code >> 12) & 0x3F));
        out += (char)(0x80 | ((code >> 6) & 0x3F));
        out += (char)(0x80 | (code & 0x3F));
    }
}

bool parse_string(const char* p, const char* end, std::string& out) {
    if (p == end || *p++ != '"') return false;
    while (p < end && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c < 0x20) return false;
        if (c != '\\') {
            out += (char)c;
            continue;
        }
        if (p == end) return false;
        char e = *p++;
        switch (e) {
            case '"': case '\\': case '/': out += e; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned code = 0;
                if (!hex4(p, end, code)) return false;
                if (code >= 0xD800 && code < 0xDC00) {   // a surrogate pair
                    unsigned low = 0;
                    if (end - p < 2 || p[0] != '\\' || p[1] != 'u') return false;
                    p += 2;
                    if (!hex4(p, end, low) || low < 0xDC00 || low >= 0xE000) return false;
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                } else if (code >= 0xDC00 && code < 0xE000) {
                    return false;
                }
                append_utf8(out, code);
                break;
            }
            default: return false;
        }
    }
    return p + 1 == end && *p == '"';
}

}  // namespace

bool parse(const std::string& text, Rml::Variant& out) {
    const char* p = text.data();
    const char* end = p + text.size();
    if (text == "true" || text == "false") {
        out = text == "true";
        return true;
    }
    if (p < end && *p == '"') {
        std::string value;
        if (!parse_string(p, end, value)) return false;
        out = value;
        return true;
    }
    // Not strtod: that follows the locale, and reads "0.42" as 0 on a Russian machine.
    double number = 0.0;
    auto result = std::from_chars(p, end, number);
    if (result.ec != std::errc() || result.ptr != end || !std::isfinite(number)) return false;
    out = number;
    return true;
}

std::string quote(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char escaped[8];
                    snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    out += escaped;
                } else {
                    out += (char)c;
                }
        }
    }
    return out + "\"";
}

std::string write(const Rml::Variant& value) {
    switch (value.GetType()) {
        case Rml::Variant::BOOL: return value.Get<bool>() ? "true" : "false";
        case Rml::Variant::STRING: return quote(value.Get<Rml::String>());
        case Rml::Variant::BYTE:
        case Rml::Variant::CHAR:
        case Rml::Variant::FLOAT:
        case Rml::Variant::DOUBLE:
        case Rml::Variant::INT:
        case Rml::Variant::INT64:
        case Rml::Variant::UINT:
        case Rml::Variant::UINT64: {
            double number = value.Get<double>();
            if (!std::isfinite(number)) return "null";
            char text[32];
            auto result = std::to_chars(text, text + sizeof(text), number);
            return std::string(text, result.ptr);
        }
        default: return "null";
    }
}

}  // namespace json_scalar
