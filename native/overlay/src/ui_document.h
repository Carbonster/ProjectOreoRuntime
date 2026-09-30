// The generic side of the overlay: it draws what the mod described and decides nothing.
//
// The mod ships a JSON file; the Runtime's Python side reads it, validates it and hands the
// result over the pipe as one `UI {...}` line. Everything after that is this file: keep the
// document, keep the values the mod sends by name, and draw.
//
// Why the document arrives as JSON rather than being read from disk here: parsing text in C++
// is miserable, and the file format has to be free to change without touching the renderer.
// Only the wire messages are a contract between the two sides, and they are small.
//
// Nothing in here knows about fishing, or about any other mod.
#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"

namespace oreoui {

enum class Shape { Text, Rect, Line, Circle, Quad, Diamond };
enum class Anchor { Left, Right, Center };
enum class VAnchor { Top, Center, Bottom };

// Things the player can press, drag and type into. None means the element is a plain shape.
// Drawn by ImGui's own widgets - the overlay only decides where they go and what they look like.
enum class Widget { None, Button, Toggle, Slider, Dropdown, Input };

// The colours a widget can be dressed in, under our names rather than ImGui's. The file is a
// public format and has to outlive whichever library happens to draw it, so ImGuiCol_* never
// appears in it; the mapping onto ImGui lives in one place, in the renderer.
enum StyleColour {
    StyleBackground, StyleHover, StyleActive, StyleText, StyleAccent,
    StyleBorder, StylePopup, StyleHint, StyleFocus, StyleColourCount
};

struct WidgetStyle {
    bool has[StyleColourCount] = {};
    ImU32 colour[StyleColourCount] = {};
    float radius = -1.0f;      // negative: leave ImGui's own
    float thickness = -1.0f;   // border width; negative: leave ImGui's own
};

// A widget's current value. Which field means anything depends on the widget: a toggle is a
// boolean, a slider a number, a dropdown and an input a string.
struct WidgetValue {
    bool boolean = false;
    float number = 0.0f;
    std::string text;
};

// A number in the document is either fixed, or supplied by the mod at run time under a name.
// `low`/`high` map the incoming 0..1 onto part of the screen, so the mod sends meaning
// ("the fish is 42% down") and the file decides where that lands.
struct Value {
    bool bound = false;
    float fixed = 0.0f;
    std::string name;
    float low = 0.0f, high = 1.0f;

    float resolve(const std::unordered_map<std::string, float>& numbers) const;
};

struct Element {
    std::string id;
    Shape shape = Shape::Rect;
    bool visible = true;

    // Geometry, in fractions of the screen box. Which of these are used depends on the shape;
    // the Python side has already checked that the required ones are present.
    Value x, y, x2, y2, x3, y3, x4, y4, w, h, r;
    bool centred = false;          // rect only: (x, y) is the middle
    bool bottom_anchored = false;  // rect only: (x, y) is the bottom-left, grows upward
    float radius = 0.0f;       // rect corner rounding, in pixels
    float thickness = 1.0f;
    float text_size = 16.0f;

    bool has_fill = false, has_border = false, has_colour = false;
    ImU32 fill = 0, border = 0, colour = 0;

    // Literal text, or the name of a string the mod supplies.
    std::string text;
    bool text_bound = false;

    // Everything below is for widgets only; a shape leaves it at the defaults. `x`, `y`, `w`
    // and `h` above place the widget, `text_size` sizes its text, and an `h` of zero means
    // "as tall as the text needs".
    Widget widget = Widget::None;
    bool enabled = true;
    std::string label;          // button caption, text beside a toggle
    std::string hint;           // input: shown while empty
    std::string suffix;         // slider: drawn after the number, "%" for a percentage
    float min = 0.0f, max = 1.0f, step = 0.0f;   // slider; a step of zero means continuous
    int decimals = 0;           // slider
    int max_length = 256;       // input, in bytes
    std::vector<std::string> options;            // dropdown
    WidgetStyle style;
    // Reset to what the file says on every load. The mod's side keeps the live value and
    // replays it after the document, the same way it replays visibility.
    WidgetValue value;
    // Input only: the buffer ImGui edits in place, max_length + 1 bytes.
    std::string edit;
};

struct Screen {
    std::string id;
    float width = 0.0f, height = 0.0f;   // pixels: a HUD keeps its size on a 4K monitor
    Anchor anchor = Anchor::Right;
    VAnchor valign = VAnchor::Center;     // independent of anchor; defaults to how it always was
    float inset_x = 0.0f, inset_y = 0.0f;
    bool visible = false;                // the mod decides when a screen appears
    std::vector<Element> elements;
    // The escape hatch: shapes the mod sent directly, drawn above the described ones. Empty
    // for every screen that never uses it, which is the normal case.
    std::vector<Element> raw;
};

// One baked font size. Only a handful are baked, because rebuilding the atlas cannot happen
// mid-frame; text asks for any size it likes and is drawn from the nearest one, scaled.
struct FontFace {
    ImFont* font = nullptr;
    float size = 0.0f;
};

// What one frame actually cost in elements. Read by the overlay's own diagnostics panel, not
// by any mod: a document of thirty elements and one of a thousand are different stories, and
// the frame time alone does not tell them apart.
struct DrawStats {
    int screens_visible = 0;
    int drawn = 0;
    int hidden = 0;
    // A visible screen has a widget on it: something the player is meant to point at.
    bool interactive = false;
};

// What happened to a value the mod tried to set.
enum class SetResult {
    Ok,
    Unknown,     // no widget by that id
    WrongType,   // a number for a toggle, a string for a slider, JSON that does not parse
    Busy,        // the player is holding this widget right now; their hand wins
};

// The whole interface, plus the live values. Guarded by its own lock: the pipe thread writes
// while the render thread reads, every frame.
class Document {
public:
    // Replaces everything. Called for each `UI` message, including after a reconnect - the mod
    // resends the document because we only ever held it in memory.
    bool load(const char* json, std::string& error);

    void set_number(const std::string& name, float value);
    void set_text(const std::string& name, const std::string& value);
    // Addresses a screen or an element by id. Returns false for an unknown name so the caller
    // can log it: a mod-side typo should be loud, not silently ignored.
    bool set_visible(const std::string& name, bool visible);

    // Moves a screen without reloading the document. This is what a "move the panel" hotkey
    // ends up calling: the mod reads the key, remembers the new position in its own settings,
    // and says where the box goes. The file still owns everything inside the box.
    bool set_placement(const std::string& screen_id, float inset_x, float inset_y,
                       const char* anchor);

    // The escape hatch. `json` is an array of shapes in the same form the file uses, drawn on
    // top of the named screen's own elements. Replaces whatever was there; an empty array
    // clears it. For the things a described file cannot say - a plotted curve, a debug scribble
    // - and deliberately second in the documentation, because a modder who reaches for this
    // first never discovers that the interface can be a file at all.
    bool set_raw(const std::string& screen_id, const char* json, std::string& error);

    // Widgets. `json` is one JSON value - true, 0.5, "text" - of the kind the widget holds.
    // A slider's number is clamped to its range and an input's text cut to its length here,
    // so nothing the mod sends can put a widget into a state the player could not.
    SetResult set_widget_value(const std::string& id, const char* json);
    // A dropdown's choices, as a JSON array of strings. The current value is kept even when it
    // is no longer among them: the mod decides what the selection becomes, not the renderer.
    bool set_options(const std::string& id, const char* json, std::string& error);
    bool set_enabled(const std::string& id, bool enabled);
    // For tests, and for nothing else: the renderer reads values while drawing.
    bool widget_value(const std::string& id, WidgetValue& out) const;

    bool empty() const;
    // Text asks for a size and is drawn from whichever baked face is closest, scaled - which is
    // why a document should stick to sizes near one of them. `faces` may be a single face; it
    // must not be empty.
    //
    // Needs a live ImGui frame. What the player did to a widget comes back as lines for the
    // pipe, appended to `events`: "EV click <id>", "EV change <id> <json>", "EV submit <id>
    // <json>".
    DrawStats draw(float screen_w, float screen_h, const FontFace* faces, int face_count,
                   std::vector<std::string>& events);

private:
    // The pipe thread writes while the render thread reads, every frame. Held only for the
    // length of a draw, which is a few hundred ImGui calls and no I/O.
    mutable std::mutex lock_;
    std::vector<Screen> screens_;
    std::unordered_map<std::string, float> numbers_;
    std::unordered_map<std::string, std::string> texts_;
    // The widget the player is holding - dragging, typing into - as of the last frame drawn.
    // A value from the mod for that one widget is refused until they let go.
    std::string active_widget_;

    Element* find_widget(const std::string& id);
    const Element* find_widget(const std::string& id) const;
};

// Exposed for testing: how the overlay writes a string into an event line.
std::string json_quote(const std::string& text);

// Exposed for testing the parser without an ImGui context.
bool parse_colour(const std::string& text, ImU32& out);

}  // namespace oreoui
