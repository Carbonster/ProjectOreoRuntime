#include "ui_document.h"

#include <charconv>
#include <cmath>
#include <cstring>
#include <mutex>

#include "log.h"

namespace oreoui {

namespace {

// ---- a small JSON reader --------------------------------------------------------------
//
// Hand-written rather than vendored: the only thing that ever writes here is our own Python
// side, so the shapes are known, and a dependency added to a DLL injected into someone else's
// game has to earn its place. It still has to survive garbage - any process running as this
// user can write to the pipe - so every step is bounds-checked and nothing recurses without
// a depth limit.

struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<Json> items;                        // Array
    std::vector<std::pair<std::string, Json>> members;  // Object, in file order

    const Json* find(const char* key) const {
        for (const auto& m : members)
            if (m.first == key) return &m.second;
        return nullptr;
    }
    bool is(Kind k) const { return kind == k; }
};

const int kMaxDepth = 32;   // documents are two or three levels deep; this is a stack guard

class Reader {
public:
    Reader(const char* text) : p_(text) {}

    bool parse(Json& out) { skip(); return value(out, 0) && (skip(), *p_ == 0); }
    const char* where() const { return p_; }

private:
    const char* p_;

    void skip() {
        while (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r') ++p_;
    }

    bool literal(const char* word) {
        size_t n = strlen(word);
        if (strncmp(p_, word, n) != 0) return false;
        p_ += n;
        return true;
    }

    bool string(std::string& out) {
        if (*p_ != '"') return false;
        ++p_;
        out.clear();
        while (*p_ && *p_ != '"') {
            if (*p_ != '\\') { out.push_back(*p_++); continue; }
            ++p_;
            switch (*p_) {
                case '"':  out.push_back('"');  ++p_; break;
                case '\\': out.push_back('\\'); ++p_; break;
                case '/':  out.push_back('/');  ++p_; break;
                case 'b':  out.push_back('\b'); ++p_; break;
                case 'f':  out.push_back('\f'); ++p_; break;
                case 'n':  out.push_back('\n'); ++p_; break;
                case 'r':  out.push_back('\r'); ++p_; break;
                case 't':  out.push_back('\t'); ++p_; break;
                case 'u': {
                    // Python escapes control characters this way even with ensure_ascii off.
                    unsigned code = 0;
                    for (int i = 1; i <= 4; ++i) {
                        char c = p_[i];
                        if (!c) return false;
                        code <<= 4;
                        if (c >= '0' && c <= '9') code |= (unsigned)(c - '0');
                        else if (c >= 'a' && c <= 'f') code |= (unsigned)(c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') code |= (unsigned)(c - 'A' + 10);
                        else return false;
                    }
                    p_ += 5;
                    // Encoded as UTF-8; surrogate pairs are not produced by our writer.
                    if (code < 0x80) {
                        out.push_back((char)code);
                    } else if (code < 0x800) {
                        out.push_back((char)(0xC0 | (code >> 6)));
                        out.push_back((char)(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back((char)(0xE0 | (code >> 12)));
                        out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back((char)(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default: return false;
            }
        }
        if (*p_ != '"') return false;
        ++p_;
        return true;
    }

    bool number(double& out) {
        // from_chars, never strtod: strtod follows LC_NUMERIC, and under a Russian locale it
        // reads "0.42" as 0. That bug appears only on the player's machine, never on ours.
        const char* start = p_;
        if (*p_ == '-' || *p_ == '+') ++p_;
        while ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' || *p_ == 'E' ||
               ((*p_ == '-' || *p_ == '+') && (p_[-1] == 'e' || p_[-1] == 'E')))
            ++p_;
        if (p_ == start) return false;
        auto result = std::from_chars(start, p_, out);
        return result.ec == std::errc() && result.ptr == p_;
    }

    bool value(Json& out, int depth) {
        if (depth > kMaxDepth) return false;
        skip();
        switch (*p_) {
            case '{': {
                out.kind = Json::Kind::Object;
                ++p_; skip();
                if (*p_ == '}') { ++p_; return true; }
                for (;;) {
                    skip();
                    std::string key;
                    if (!string(key)) return false;
                    skip();
                    if (*p_ != ':') return false;
                    ++p_;
                    Json child;
                    if (!value(child, depth + 1)) return false;
                    out.members.emplace_back(std::move(key), std::move(child));
                    skip();
                    if (*p_ == ',') { ++p_; continue; }
                    if (*p_ == '}') { ++p_; return true; }
                    return false;
                }
            }
            case '[': {
                out.kind = Json::Kind::Array;
                ++p_; skip();
                if (*p_ == ']') { ++p_; return true; }
                for (;;) {
                    Json child;
                    if (!value(child, depth + 1)) return false;
                    out.items.push_back(std::move(child));
                    skip();
                    if (*p_ == ',') { ++p_; continue; }
                    if (*p_ == ']') { ++p_; return true; }
                    return false;
                }
            }
            case '"':
                out.kind = Json::Kind::String;
                return string(out.text);
            case 't':
                out.kind = Json::Kind::Bool; out.boolean = true;  return literal("true");
            case 'f':
                out.kind = Json::Kind::Bool; out.boolean = false; return literal("false");
            case 'n':
                out.kind = Json::Kind::Null; return literal("null");
            default:
                out.kind = Json::Kind::Number;
                return number(out.number);
        }
    }
};

// ---- document mapping -----------------------------------------------------------------

bool shape_from_name(const std::string& name, Shape& out) {
    if (name == "text")    { out = Shape::Text;    return true; }
    if (name == "rect")    { out = Shape::Rect;    return true; }
    if (name == "line")    { out = Shape::Line;    return true; }
    if (name == "circle")  { out = Shape::Circle;  return true; }
    if (name == "quad")    { out = Shape::Quad;    return true; }
    if (name == "diamond") { out = Shape::Diamond; return true; }
    return false;
}

bool widget_from_name(const std::string& name, Widget& out) {
    if (name == "button")   { out = Widget::Button;   return true; }
    if (name == "toggle")   { out = Widget::Toggle;   return true; }
    if (name == "slider")   { out = Widget::Slider;   return true; }
    if (name == "dropdown") { out = Widget::Dropdown; return true; }
    if (name == "input")    { out = Widget::Input;    return true; }
    return false;
}

// The names a file uses for a widget's colours, in StyleColour order.
const char* const kStyleColourNames[StyleColourCount] = {
    "background", "hover", "active", "text", "accent", "border", "popup", "hint", "focus",
};

Anchor anchor_from_name(const std::string& name) {
    if (name == "left") return Anchor::Left;
    if (name == "center") return Anchor::Center;
    return Anchor::Right;
}

VAnchor valign_from_name(const std::string& name) {
    if (name == "top") return VAnchor::Top;
    if (name == "bottom") return VAnchor::Bottom;
    return VAnchor::Center;
}

// A field is a plain number, or `{"bind": name, "from": lo, "to": hi}`.
Value read_value(const Json* raw, float fallback = 0.0f) {
    Value v;
    v.fixed = fallback;
    if (!raw) return v;
    if (raw->is(Json::Kind::Number)) {
        v.fixed = (float)raw->number;
        return v;
    }
    if (raw->is(Json::Kind::Object)) {
        const Json* name = raw->find("bind");
        if (name && name->is(Json::Kind::String) && !name->text.empty()) {
            v.bound = true;
            v.name = name->text;
            const Json* lo = raw->find("from");
            const Json* hi = raw->find("to");
            v.low = lo && lo->is(Json::Kind::Number) ? (float)lo->number : 0.0f;
            v.high = hi && hi->is(Json::Kind::Number) ? (float)hi->number : 1.0f;
        }
    }
    return v;
}

float read_float(const Json* raw, float fallback) {
    return raw && raw->is(Json::Kind::Number) ? (float)raw->number : fallback;
}

bool read_colour(const Json* raw, ImU32& out) {
    if (!raw || !raw->is(Json::Kind::String)) return false;
    return parse_colour(raw->text, out);
}

std::string read_string(const Json* raw) {
    return raw && raw->is(Json::Kind::String) ? raw->text : std::string();
}

// Cut to at most `limit` bytes without splitting a UTF-8 character. A half character at the
// end of an input field would be drawn as garbage and sent back to the mod as invalid UTF-8.
void truncate_utf8(std::string& text, size_t limit) {
    if (text.size() <= limit) return;
    size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    text.resize(end);
}

float clamp_to_range(float value, float low, float high) {
    if (!(value >= low)) return low;   // also catches NaN
    if (value > high) return high;
    return value;
}

// An input's editing buffer holds the value and room for the longest text allowed.
void sync_edit_buffer(Element& e) {
    e.edit.assign((size_t)e.max_length + 1, '\0');
    memcpy(&e.edit[0], e.value.text.data(), e.value.text.size());
}

// The widget half of an element. Like the shapes, the Python side has already checked every
// field, so this only has to survive garbage, not explain it.
void read_widget(const Json& e, Element& out) {
    out.label = read_string(e.find("label"));
    out.hint = read_string(e.find("hint"));
    out.suffix = read_string(e.find("suffix"));
    const Json* enabled = e.find("enabled");
    out.enabled = enabled && enabled->is(Json::Kind::Bool) ? enabled->boolean : true;

    out.min = read_float(e.find("min"), 0.0f);
    out.max = read_float(e.find("max"), 1.0f);
    if (!(out.max > out.min)) out.max = out.min + 1.0f;
    out.step = read_float(e.find("step"), 0.0f);
    if (!(out.step > 0.0f)) out.step = 0.0f;
    out.decimals = (int)read_float(e.find("decimals"), 0.0f);
    if (out.decimals < 0) out.decimals = 0;
    if (out.decimals > 6) out.decimals = 6;
    out.max_length = (int)read_float(e.find("maxLength"), 256.0f);
    if (out.max_length < 1) out.max_length = 1;
    if (out.max_length > 4096) out.max_length = 4096;

    const Json* options = e.find("options");
    if (options && options->is(Json::Kind::Array))
        for (const Json& option : options->items)
            if (option.is(Json::Kind::String)) out.options.push_back(option.text);

    const Json* style = e.find("style");
    if (style && style->is(Json::Kind::Object)) {
        for (int i = 0; i < StyleColourCount; ++i)
            out.style.has[i] = read_colour(style->find(kStyleColourNames[i]), out.style.colour[i]);
        out.style.radius = read_float(style->find("radius"), -1.0f);
        out.style.thickness = read_float(style->find("thickness"), -1.0f);
    }

    const Json* value = e.find("value");
    switch (out.widget) {
        case Widget::Toggle:
            out.value.boolean = value && value->is(Json::Kind::Bool) && value->boolean;
            break;
        case Widget::Slider:
            out.value.number = clamp_to_range(
                value && value->is(Json::Kind::Number) ? (float)value->number : out.min,
                out.min, out.max);
            break;
        case Widget::Dropdown:
            if (value && value->is(Json::Kind::String)) out.value.text = value->text;
            else if (!out.options.empty()) out.value.text = out.options[0];
            break;
        case Widget::Input:
            out.value.text = read_string(value);
            truncate_utf8(out.value.text, (size_t)out.max_length);
            sync_edit_buffer(out);
            break;
        default:
            break;
    }
}

// One element, in the same form whether it came from the file or straight down the escape
// hatch. Returns false only for a shape or widget nobody knows; everything else the Python side
// has already checked, and re-checking it here would be two validators to keep in step.
//
// The escape hatch passes `allow_widgets` false: a widget there would be a control that
// disappears whenever the mod redraws the layer, with its value going with it.
bool read_element(const Json& e, Element& out, bool allow_widgets) {
    const Json* eid = e.find("id");
    if (eid && eid->is(Json::Kind::String)) out.id = eid->text;
    const Json* widget = e.find("widget");
    if (widget) {
        if (!allow_widgets || !widget->is(Json::Kind::String) ||
            !widget_from_name(widget->text, out.widget))
            return false;
    } else {
        const Json* shape = e.find("shape");
        if (!shape || !shape->is(Json::Kind::String) || !shape_from_name(shape->text, out.shape))
            return false;
    }
    const Json* evis = e.find("visible");
    out.visible = evis && evis->is(Json::Kind::Bool) ? evis->boolean : true;

    out.x = read_value(e.find("x"));
    out.y = read_value(e.find("y"));
    out.x2 = read_value(e.find("x2"));
    out.y2 = read_value(e.find("y2"));
    out.x3 = read_value(e.find("x3"));
    out.y3 = read_value(e.find("y3"));
    out.x4 = read_value(e.find("x4"));
    out.y4 = read_value(e.find("y4"));
    out.w = read_value(e.find("w"));
    out.h = read_value(e.find("h"));
    out.r = read_value(e.find("r"));
    out.radius = read_float(e.find("radius"), 0.0f);
    out.thickness = read_float(e.find("thickness"), 1.0f);
    out.text_size = read_float(e.find("size"), 16.0f);

    const Json* origin = e.find("origin");
    if (origin && origin->is(Json::Kind::String)) {
        out.centred = origin->text == "center";
        out.bottom_anchored = origin->text == "bottom-left";
    }

    out.has_fill = read_colour(e.find("fill"), out.fill);
    out.has_border = read_colour(e.find("border"), out.border);
    out.has_colour = read_colour(e.find("color"), out.colour);

    const Json* text = e.find("text");
    if (text) {
        if (text->is(Json::Kind::String)) {
            out.text = text->text;
        } else if (text->is(Json::Kind::Object)) {
            const Json* bind = text->find("bind");
            if (bind && bind->is(Json::Kind::String)) {
                out.text = bind->text;
                out.text_bound = true;
            }
        }
    }
    if (out.widget != Widget::None) read_widget(e, out);
    return true;
}

void append_number(std::string& out, double value) {
    // to_chars, for the same reason the reader uses from_chars: sprintf follows the locale and
    // would write 0,5 on a Russian machine, which the Python side then cannot read.
    char buffer[32];
    auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ec == std::errc() ? result.ptr : buffer);
}

}  // namespace

std::string json_quote(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (char c : text) {
        unsigned char u = static_cast<unsigned char>(c);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (u < 0x20) {
            // A newline in an event would end the line and turn the rest into a message of its
            // own - the same trap TXT avoids on the way in.
            static const char* hex = "0123456789abcdef";
            out += "\\u00";
            out.push_back(hex[u >> 4]);
            out.push_back(hex[u & 0x0F]);
        } else {
            out.push_back(c);   // UTF-8 goes through as it is
        }
    }
    out.push_back('"');
    return out;
}

// `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`. Palette names are already resolved by the Python
// side, so anything that reaches here is a literal.
bool parse_colour(const std::string& text, ImU32& out) {
    if (text.size() < 4 || text[0] != '#') return false;
    auto nibble = [](char c, int& value) {
        if (c >= '0' && c <= '9') { value = c - '0'; return true; }
        if (c >= 'a' && c <= 'f') { value = c - 'a' + 10; return true; }
        if (c >= 'A' && c <= 'F') { value = c - 'A' + 10; return true; }
        return false;
    };
    int digits[8] = {0};
    size_t count = text.size() - 1;
    if (count != 3 && count != 4 && count != 6 && count != 8) return false;
    for (size_t i = 0; i < count; ++i)
        if (!nibble(text[i + 1], digits[i])) return false;

    int r, g, b, a = 255;
    if (count <= 4) {                       // shorthand: each digit doubled, as in CSS
        r = digits[0] * 17; g = digits[1] * 17; b = digits[2] * 17;
        if (count == 4) a = digits[3] * 17;
    } else {
        r = digits[0] * 16 + digits[1];
        g = digits[2] * 16 + digits[3];
        b = digits[4] * 16 + digits[5];
        if (count == 8) a = digits[6] * 16 + digits[7];
    }
    out = IM_COL32(r, g, b, a);
    return true;
}

float Value::resolve(const std::unordered_map<std::string, float>& numbers) const {
    if (!bound) return fixed;
    auto it = numbers.find(name);
    // A bound value the mod has not sent yet sits at the start of its range rather than at
    // zero: for a bar mapped into the middle of a screen, zero would be off the panel.
    float t = it == numbers.end() ? 0.0f : it->second;
    return low + t * (high - low);
}

// ---- Document -------------------------------------------------------------------------

bool Document::load(const char* json, std::string& error) {
    Json root;
    Reader reader(json);
    if (!reader.parse(root) || !root.is(Json::Kind::Object)) {
        error = "the UI document is not valid JSON";
        return false;
    }
    const Json* screens = root.find("screens");
    if (!screens || !screens->is(Json::Kind::Array)) {
        error = "the UI document has no \"screens\" array";
        return false;
    }

    std::vector<Screen> built;
    built.reserve(screens->items.size());
    for (const Json& raw : screens->items) {
        if (!raw.is(Json::Kind::Object)) continue;
        Screen screen;
        const Json* id = raw.find("id");
        if (id && id->is(Json::Kind::String)) screen.id = id->text;
        const Json* size = raw.find("size");
        if (size && size->is(Json::Kind::Array) && size->items.size() == 2) {
            screen.width = (float)size->items[0].number;
            screen.height = (float)size->items[1].number;
        }
        const Json* anchor = raw.find("anchor");
        if (anchor && anchor->is(Json::Kind::String)) screen.anchor = anchor_from_name(anchor->text);
        const Json* valign = raw.find("valign");
        if (valign && valign->is(Json::Kind::String)) screen.valign = valign_from_name(valign->text);
        const Json* inset = raw.find("inset");
        if (inset && inset->is(Json::Kind::Array) && inset->items.size() == 2) {
            screen.inset_x = (float)inset->items[0].number;
            screen.inset_y = (float)inset->items[1].number;
        }
        const Json* visible = raw.find("visible");
        screen.visible = visible && visible->is(Json::Kind::Bool) ? visible->boolean : false;

        const Json* elements = raw.find("elements");
        if (elements && elements->is(Json::Kind::Array)) {
            for (const Json& e : elements->items) {
                if (!e.is(Json::Kind::Object)) continue;
                Element element;
                if (!read_element(e, element, true)) {
                    LOG_WARN("ui: element '%s' has an unknown shape or widget, skipped",
                             element.id.c_str());
                    continue;
                }
                screen.elements.push_back(std::move(element));
            }
        }

        built.push_back(std::move(screen));
    }

    size_t count;
    {
        std::lock_guard<std::mutex> guard(lock_);
        screens_ = std::move(built);
        // Values are deliberately kept across a reload. The mod resends the document when the
        // pipe reconnects, and dropping what it last told us would blank the panel for a frame
        // or two until the next update of every single value.
        //
        // Widget values are not: they arrive at what the file says, and the mod's side replays
        // what it holds right after the document. Keeping them here instead would leave a mod
        // that restarts inside a running game looking at the old values while believing the
        // file's - two sides of one widget disagreeing, with nothing to tell them apart.
        count = screens_.size();
    }
    // Counted inside the lock. Reading it outside was a race with the render thread for the
    // sake of one log line - harmless in practice, wrong in principle, and exactly the kind of
    // thing that is impossible to find later.
    LOG("ui: document loaded, %zu screen(s)", count);
    return true;
}

void Document::set_number(const std::string& name, float value) {
    std::lock_guard<std::mutex> guard(lock_);
    numbers_[name] = value;
}

void Document::set_text(const std::string& name, const std::string& value) {
    std::lock_guard<std::mutex> guard(lock_);
    texts_[name] = value;
}

bool Document::set_visible(const std::string& name, bool visible) {
    std::lock_guard<std::mutex> guard(lock_);
    for (Screen& screen : screens_) {
        if (screen.id == name) { screen.visible = visible; return true; }
        for (Element& element : screen.elements)
            if (element.id == name) { element.visible = visible; return true; }
    }
    return false;
}

bool Document::empty() const {
    std::lock_guard<std::mutex> guard(lock_);
    return screens_.empty();
}

bool Document::set_placement(const std::string& screen_id, float inset_x, float inset_y,
                             const char* anchor) {
    std::lock_guard<std::mutex> guard(lock_);
    for (Screen& screen : screens_) {
        if (screen.id != screen_id) continue;
        screen.inset_x = inset_x;
        screen.inset_y = inset_y;
        if (anchor && *anchor) screen.anchor = anchor_from_name(anchor);
        return true;
    }
    return false;
}

bool Document::set_raw(const std::string& screen_id, const char* json, std::string& error) {
    Json root;
    Reader reader(json);
    if (!reader.parse(root) || !root.is(Json::Kind::Array)) {
        error = "raw shapes must be a JSON array";
        return false;
    }

    std::vector<Element> shapes;
    shapes.reserve(root.items.size());
    for (const Json& e : root.items) {
        if (!e.is(Json::Kind::Object)) continue;
        Element element;
        if (!read_element(e, element, false)) {
            error = "a raw shape has an unknown \"shape\", or is a widget";
            return false;
        }
        shapes.push_back(std::move(element));
    }

    std::lock_guard<std::mutex> guard(lock_);
    for (Screen& screen : screens_) {
        if (screen.id != screen_id) continue;
        screen.raw = std::move(shapes);
        return true;
    }
    error = "no screen named '" + screen_id + "'";
    return false;
}

Element* Document::find_widget(const std::string& id) {
    for (Screen& screen : screens_)
        for (Element& element : screen.elements)
            if (element.widget != Widget::None && element.id == id) return &element;
    return nullptr;
}

const Element* Document::find_widget(const std::string& id) const {
    return const_cast<Document*>(this)->find_widget(id);
}

SetResult Document::set_widget_value(const std::string& id, const char* json) {
    Json value;
    Reader reader(json);
    bool parsed = reader.parse(value);

    std::lock_guard<std::mutex> guard(lock_);
    Element* e = find_widget(id);
    if (!e) return SetResult::Unknown;
    if (!parsed) return SetResult::WrongType;
    if (id == active_widget_) return SetResult::Busy;
    switch (e->widget) {
        case Widget::Toggle:
            if (!value.is(Json::Kind::Bool)) return SetResult::WrongType;
            e->value.boolean = value.boolean;
            return SetResult::Ok;
        case Widget::Slider:
            if (!value.is(Json::Kind::Number)) return SetResult::WrongType;
            e->value.number = clamp_to_range((float)value.number, e->min, e->max);
            return SetResult::Ok;
        case Widget::Dropdown:
            if (!value.is(Json::Kind::String)) return SetResult::WrongType;
            e->value.text = value.text;
            return SetResult::Ok;
        case Widget::Input:
            if (!value.is(Json::Kind::String)) return SetResult::WrongType;
            e->value.text = value.text;
            truncate_utf8(e->value.text, (size_t)e->max_length);
            sync_edit_buffer(*e);
            return SetResult::Ok;
        default:
            // A button holds nothing to set.
            return SetResult::WrongType;
    }
}

bool Document::set_options(const std::string& id, const char* json, std::string& error) {
    Json root;
    Reader reader(json);
    if (!reader.parse(root) || !root.is(Json::Kind::Array)) {
        error = "options must be a JSON array of strings";
        return false;
    }
    std::vector<std::string> options;
    options.reserve(root.items.size());
    for (const Json& item : root.items) {
        if (!item.is(Json::Kind::String)) {
            error = "options must be a JSON array of strings";
            return false;
        }
        options.push_back(item.text);
    }

    std::lock_guard<std::mutex> guard(lock_);
    Element* e = find_widget(id);
    if (!e || e->widget != Widget::Dropdown) {
        error = "no dropdown named '" + id + "'";
        return false;
    }
    e->options = std::move(options);
    return true;
}

bool Document::set_enabled(const std::string& id, bool enabled) {
    std::lock_guard<std::mutex> guard(lock_);
    Element* e = find_widget(id);
    if (!e) return false;
    e->enabled = enabled;
    return true;
}

bool Document::widget_value(const std::string& id, WidgetValue& out) const {
    std::lock_guard<std::mutex> guard(lock_);
    const Element* e = find_widget(id);
    if (!e) return false;
    out = e->value;
    return true;
}

// Only a handful of sizes are baked into the atlas and rebuilding it cannot happen mid-frame,
// so text is drawn from the nearest baked face and scaled from there. Nearest rather than
// "small below a threshold, large above": scaling a 34px string up from a 20px face is what
// made the fish name look like mud, and the fix is to have something close to ask for.
static const FontFace* nearest_face(const FontFace* faces, int count, float wanted) {
    const FontFace* best = nullptr;
    float bestGap = 0.0f;
    for (int i = 0; i < count; ++i) {
        if (!faces[i].font) continue;
        float gap = faces[i].size - wanted;
        if (gap < 0.0f) gap = -gap;
        if (!best || gap < bestGap) { best = &faces[i]; bestGap = gap; }
    }
    return best;
}

namespace {

// A screen's box in pixels. Fractions of the box become pixels here, and only here. The mod
// works in meaning, the file works in fractions, the renderer works in pixels.
struct Box {
    float ox, oy, w, h;
    const std::unordered_map<std::string, float>* numbers;

    float px(const Value& v) const { return ox + v.resolve(*numbers) * w; }
    float py(const Value& v) const { return oy + v.resolve(*numbers) * h; }
    float sx(const Value& v) const { return v.resolve(*numbers) * w; }
    float sy(const Value& v) const { return v.resolve(*numbers) * h; }
};

void draw_shape(ImDrawList* dl, const Box& b, const Element& e,
                const std::unordered_map<std::string, std::string>& texts,
                const FontFace* faces, int face_count) {
    switch (e.shape) {
        case Shape::Rect: {
            float w = b.sx(e.w), h = b.sy(e.h);
            float x = b.px(e.x), y = b.py(e.y);
            if (e.centred) { x -= w * 0.5f; y -= h * 0.5f; }
            else if (e.bottom_anchored) y -= h;   // pinned at the bottom, grows upward
            ImVec2 p0(x, y), p1(x + w, y + h);
            if (e.has_fill) dl->AddRectFilled(p0, p1, e.fill, e.radius);
            if (e.has_border) dl->AddRect(p0, p1, e.border, e.radius, 0, e.thickness);
            break;
        }
        case Shape::Text: {
            if (!e.has_colour) break;
            const std::string* shown = &e.text;
            if (e.text_bound) {
                auto it = texts.find(e.text);
                if (it == texts.end()) break;   // nothing sent yet: draw nothing
                shown = &it->second;
            }
            if (shown->empty()) break;
            const FontFace* face = nearest_face(faces, face_count, e.text_size);
            dl->AddText(face ? face->font : nullptr, e.text_size,
                        ImVec2(b.px(e.x), b.py(e.y)), e.colour, shown->c_str());
            break;
        }
        case Shape::Line: {
            if (!e.has_colour) break;
            dl->AddLine(ImVec2(b.px(e.x), b.py(e.y)), ImVec2(b.px(e.x2), b.py(e.y2)),
                        e.colour, e.thickness);
            break;
        }
        case Shape::Circle: {
            ImVec2 c(b.px(e.x), b.py(e.y));
            float radius = b.sx(e.r);
            if (e.has_fill) dl->AddCircleFilled(c, radius, e.fill, 24);
            if (e.has_border) dl->AddCircle(c, radius, e.border, 24, e.thickness);
            break;
        }
        case Shape::Diamond: {
            ImVec2 c(b.px(e.x), b.py(e.y));
            // Radius in width fractions for both axes, so a diamond stays a diamond on a
            // screen box that is not square.
            float rx = b.sx(e.r), ry = b.sx(e.r);
            ImVec2 p0(c.x, c.y - ry), p1(c.x + rx, c.y);
            ImVec2 p2(c.x, c.y + ry), p3(c.x - rx, c.y);
            if (e.has_fill) dl->AddQuadFilled(p0, p1, p2, p3, e.fill);
            if (e.has_border) dl->AddQuad(p0, p1, p2, p3, e.border, e.thickness);
            break;
        }
        case Shape::Quad: {
            ImVec2 p0(b.px(e.x), b.py(e.y)), p1(b.px(e.x2), b.py(e.y2));
            ImVec2 p2(b.px(e.x3), b.py(e.y3)), p3(b.px(e.x4), b.py(e.y4));
            if (e.has_fill) dl->AddQuadFilled(p0, p1, p2, p3, e.fill);
            if (e.has_border) dl->AddQuad(p0, p1, p2, p3, e.border, e.thickness);
            break;
        }
    }
}

// Which of ImGui's colours each of ours stands for. The one place the two vocabularies meet.
struct StyleTarget {
    StyleColour ours;
    ImGuiCol theirs;
};
const StyleTarget kStyleTargets[] = {
    {StyleBackground, ImGuiCol_FrameBg},        {StyleBackground, ImGuiCol_Button},
    {StyleHover, ImGuiCol_FrameBgHovered},      {StyleHover, ImGuiCol_ButtonHovered},
    {StyleHover, ImGuiCol_HeaderHovered},       // a dropdown row under the pointer
    {StyleActive, ImGuiCol_FrameBgActive},      {StyleActive, ImGuiCol_ButtonActive},
    {StyleActive, ImGuiCol_Header},             // the chosen dropdown row
    {StyleActive, ImGuiCol_HeaderActive},
    {StyleText, ImGuiCol_Text},
    {StyleAccent, ImGuiCol_SliderGrab},         {StyleAccent, ImGuiCol_SliderGrabActive},
    {StyleAccent, ImGuiCol_CheckMark},
    {StyleBorder, ImGuiCol_Border},
    {StylePopup, ImGuiCol_PopupBg},
    {StyleHint, ImGuiCol_TextDisabled},         // an input's hint is drawn in this colour
    {StyleFocus, ImGuiCol_NavCursor},           // the frame the gamepad moves around
};

// Pushes what the widget asks for and says how much to pop.
void push_style(const WidgetStyle& s, float pad_y, int& colours, int& vars) {
    colours = 0;
    vars = 0;
    for (const StyleTarget& t : kStyleTargets) {
        if (!s.has[t.ours]) continue;
        ImGui::PushStyleColor(t.theirs, s.colour[t.ours]);
        ++colours;
    }
    if (s.radius >= 0.0f) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, s.radius);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, s.radius);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, s.radius);
        vars += 3;
    }
    // A border colour on its own should show a border; ImGui's default width is zero.
    float thickness = s.thickness >= 0.0f ? s.thickness : (s.has[StyleBorder] ? 1.0f : -1.0f);
    if (thickness >= 0.0f) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, thickness);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, thickness);
        vars += 2;
    }
    if (pad_y >= 0.0f) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(ImGui::GetStyle().FramePadding.x, pad_y));
        ++vars;
    }
}

void add_event(std::vector<std::string>& events, const char* kind, const std::string& id,
               const std::string& payload) {
    std::string line = "EV ";
    line += kind;
    line += ' ';
    line += id;
    if (!payload.empty()) {
        line += ' ';
        line += payload;
    }
    events.push_back(std::move(line));
}

std::string number_payload(float value) {
    std::string out;
    append_number(out, (double)value);
    return out;
}

// One widget, drawn by ImGui inside its screen's window. Returns true while the player is
// holding it.
bool draw_widget(Element& e, const Box& b, const FontFace* faces, int face_count,
                 std::vector<std::string>& events) {
    float x = b.px(e.x), y = b.py(e.y), w = b.sx(e.w);
    float h = e.h.bound || e.h.fixed > 0.0f ? b.sy(e.h) : 0.0f;
    const FontFace* face = nearest_face(faces, face_count, e.text_size);

    ImGui::PushID(e.id.c_str());
    ImGui::PushFont(face ? face->font : nullptr, e.text_size);
    // A height from the file is met by padding the frame around the text, which is how ImGui
    // sizes every widget; no height keeps its own padding.
    float pad_y = -1.0f;
    if (h > 0.0f) {
        pad_y = (h - ImGui::GetFontSize()) * 0.5f;
        if (pad_y < 0.0f) pad_y = 0.0f;
    }
    int colours = 0, vars = 0;
    push_style(e.style, pad_y, colours, vars);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::BeginDisabled(!e.enabled);

    bool held = false;
    switch (e.widget) {
        case Widget::Button: {
            std::string label = e.label + "##w";
            if (ImGui::Button(label.c_str(), ImVec2(w, h)))
                add_event(events, "click", e.id, std::string());
            held = ImGui::IsItemActive();
            break;
        }
        case Widget::Toggle: {
            std::string label = e.label + "##w";
            bool on = e.value.boolean;
            if (ImGui::Checkbox(label.c_str(), &on) && on != e.value.boolean) {
                e.value.boolean = on;
                add_event(events, "change", e.id, on ? "true" : "false");
            }
            held = ImGui::IsItemActive();
            break;
        }
        case Widget::Slider: {
            // ImGui wants a printf pattern. It is built here from `decimals` and `suffix` so
            // the file never has to know printf exists; a literal % in the suffix is doubled.
            std::string format = "%." + std::to_string(e.decimals) + "f";
            for (char c : e.suffix) {
                if (c == '%') format += "%%";
                else format.push_back(c);
            }
            float v = e.value.number;
            ImGui::SetNextItemWidth(w);
            if (ImGui::SliderFloat("##w", &v, e.min, e.max, format.c_str(),
                                   ImGuiSliderFlags_AlwaysClamp)) {
                if (e.step > 0.0f) v = e.min + std::round((v - e.min) / e.step) * e.step;
                v = clamp_to_range(v, e.min, e.max);
                if (v != e.value.number) {
                    e.value.number = v;
                    add_event(events, "change", e.id, number_payload(v));
                }
            }
            held = ImGui::IsItemActive();
            break;
        }
        case Widget::Dropdown: {
            ImGui::SetNextItemWidth(w);
            if (ImGui::BeginCombo("##w", e.value.text.c_str())) {
                for (size_t i = 0; i < e.options.size(); ++i) {
                    const std::string& option = e.options[i];
                    bool chosen = option == e.value.text;
                    ImGui::PushID((int)i);
                    if (ImGui::Selectable(option.c_str(), chosen) && !chosen) {
                        e.value.text = option;
                        add_event(events, "change", e.id, json_quote(option));
                    }
                    if (chosen) ImGui::SetItemDefaultFocus();
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            break;
        }
        case Widget::Input: {
            if (e.edit.size() != (size_t)e.max_length + 1) sync_edit_buffer(e);
            ImGui::SetNextItemWidth(w);
            // EnterReturnsTrue only changes what the call returns: the buffer is still edited
            // live, so every keystroke is seen below and Enter is told apart from typing.
            bool entered = ImGui::InputTextWithHint("##w", e.hint.c_str(), &e.edit[0],
                                                    e.edit.size(),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
            held = ImGui::IsItemActive();
            std::string now(e.edit.c_str());
            if (now != e.value.text) {
                e.value.text = now;
                add_event(events, "change", e.id, json_quote(now));
            }
            if (entered) add_event(events, "submit", e.id, json_quote(e.value.text));
            break;
        }
        default:
            break;
    }

    ImGui::EndDisabled();
    ImGui::PopStyleVar(vars);
    ImGui::PopStyleColor(colours);
    ImGui::PopFont();
    ImGui::PopID();
    return held;
}

}  // namespace

DrawStats Document::draw(float screen_w, float screen_h, const FontFace* faces, int face_count,
                         std::vector<std::string>& events) {
    DrawStats stats;
    std::lock_guard<std::mutex> guard(lock_);
    active_widget_.clear();
    for (Screen& screen : screens_) {
        if (!screen.visible || screen.width <= 0.0f || screen.height <= 0.0f) {
            stats.hidden += (int)(screen.elements.size() + screen.raw.size());
            continue;
        }
        ++stats.screens_visible;

        float ox = screen.inset_x;
        if (screen.anchor == Anchor::Right) ox = screen_w - screen.width - screen.inset_x;
        else if (screen.anchor == Anchor::Center) ox = (screen_w - screen.width) * 0.5f + screen.inset_x;
        // Independent of ox: a screen can be pinned left and top at once, like a corner
        // dialog, or centred on one axis and pinned on the other.
        float oy = screen.inset_y;
        if (screen.valign == VAnchor::Bottom) oy = screen_h - screen.height - screen.inset_y;
        else if (screen.valign == VAnchor::Center) oy = (screen_h - screen.height) * 0.5f + screen.inset_y;
        Box box{ox, oy, screen.width, screen.height, &numbers_};

        bool interactive = false;
        for (const Element& e : screen.elements)
            if (e.visible && e.widget != Widget::None) { interactive = true; break; }

        // A screen with nothing to press is drawn exactly as before widgets existed: straight
        // onto the foreground, unclipped, taking no input. A HUD must never become something
        // the mouse can land on.
        //
        // A screen with widgets becomes an ImGui window over its box, because that is where
        // widgets live - it gives them focus, gamepad navigation, and popups that open above
        // everything. Its shapes go into the same window in file order, so a shape listed after
        // a widget draws over it, exactly as on a plain screen.
        ImDrawList* dl = nullptr;
        if (interactive) {
            stats.interactive = true;
            ImGui::SetNextWindowPos(ImVec2(ox, oy));
            ImGui::SetNextWindowSize(ImVec2(screen.width, screen.height));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
            std::string name = "##oreo_screen_" + screen.id;
            ImGui::Begin(name.c_str(), nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleVar(3);
            dl = ImGui::GetWindowDrawList();
        } else {
            dl = ImGui::GetForegroundDrawList();
        }

        // Shapes on a plain screen are never clipped to its box, and moving them into a window
        // must not start clipping them now.
        auto shape = [&](const Element& e) {
            if (interactive) dl->PushClipRectFullScreen();
            draw_shape(dl, box, e, texts_, faces, face_count);
            if (interactive) dl->PopClipRect();
        };

        for (Element& e : screen.elements) {
            if (!e.visible) { ++stats.hidden; continue; }
            ++stats.drawn;
            if (e.widget == Widget::None) shape(e);
            else if (draw_widget(e, box, faces, face_count, events)) active_widget_ = e.id;
        }
        // Whatever came down the escape hatch goes last, so a mod drawing by hand draws on top
        // of its own panel rather than under it.
        for (const Element& e : screen.raw) {
            if (!e.visible) { ++stats.hidden; continue; }
            ++stats.drawn;
            shape(e);
        }

        if (interactive) ImGui::End();
    }
    return stats;
}

}  // namespace oreoui
