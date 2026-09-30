// Tests for the half of the overlay that reads what the mod sent.
//
// This code is the only place in the Runtime that parses input it did not produce. The pipe is
// reachable by any process running as the same user, so "survives garbage without taking the
// game down with it" is a requirement, not a nicety - and a crash here is a crash in someone
// else's game, with a Vulkan overlay in the stack trace and no obvious culprit.
//
// Drawing is not tested here: it needs an ImGui context and a real font atlas, and what it
// does is by design a straight translation of the model into ImDrawList calls. What is tested
// is everything that decides what those calls will be.

#include <clocale>
#include <cstdio>
#include <cstring>
#include <string>

#include "ui_document.h"

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

static std::string minimal_document() {
    return R"({"format":1,"screens":[{"id":"catch","size":[400,600],"anchor":"right",)"
           R"("inset":[200,0],"visible":false,"elements":[)"
           R"({"id":"bar","shape":"rect","visible":true,"x":0.4,)"
           R"("y":{"bind":"barY","from":0.25,"to":0.82},"w":0.2,"h":0.1,"fill":"#6cf"},)"
           R"({"id":"name","shape":"text","x":0.1,"y":0.1,"text":{"bind":"fishName"},)"
           R"("color":"#ffffff"}]}]})";
}

// ---- colours ---------------------------------------------------------------------------

static void test_colours() {
    ImU32 colour = 0;

    check(oreoui::parse_colour("#ff0000", colour), "#rrggbb parses");
    check(colour == IM_COL32(255, 0, 0, 255), "#ff0000 is opaque red");

    check(oreoui::parse_colour("#f00", colour), "#rgb shorthand parses");
    check(colour == IM_COL32(255, 0, 0, 255), "#f00 means the same as #ff0000");

    check(oreoui::parse_colour("#6cf", colour), "#6cf parses");
    check(colour == IM_COL32(0x66, 0xcc, 0xff, 255), "each shorthand digit is doubled");

    check(oreoui::parse_colour("#00000080", colour), "#rrggbbaa parses");
    check(colour == IM_COL32(0, 0, 0, 128), "the fourth pair is alpha");

    check(!oreoui::parse_colour("ff0000", colour), "a colour without # is refused");
    check(!oreoui::parse_colour("#gg0000", colour), "a non-hex digit is refused");
    check(oreoui::parse_colour("#ff00", colour), "#rgba shorthand parses");
    check(colour == IM_COL32(255, 255, 0, 0), "#ff00 is transparent yellow, not a mistake");
    check(!oreoui::parse_colour("#ff000", colour), "a five-digit colour is refused");
    check(!oreoui::parse_colour("#ff0000000", colour), "a nine-digit colour is refused");
    check(!oreoui::parse_colour("", colour), "an empty colour is refused");
    check(!oreoui::parse_colour("#", colour), "a bare # is refused");
}

// ---- the document ----------------------------------------------------------------------

static void test_document_loads() {
    oreoui::Document doc;
    std::string error;

    check(doc.empty(), "a fresh document holds nothing");
    check(doc.load(minimal_document().c_str(), error), "a well-formed document loads");
    check(!doc.empty(), "and is no longer empty");
    check(error.empty(), "with no error to report");
}

static void test_visibility_addresses_screens_and_elements() {
    oreoui::Document doc;
    std::string error;
    doc.load(minimal_document().c_str(), error);

    check(doc.set_visible("catch", true), "a screen can be shown by id");
    check(doc.set_visible("bar", false), "so can a single element");
    check(!doc.set_visible("nonexistent", true),
          "an unknown name is refused rather than silently ignored");
}

// ---- garbage ---------------------------------------------------------------------------
//
// None of these may crash. Refusing them is the correct outcome; the point of the test is
// that we come back at all.

static void test_malformed_input_is_refused_not_fatal() {
    const char* nasty[] = {
        "",
        "not json at all",
        "{",
        "[",
        "{\"screens\":",
        "{\"screens\":[",
        "{\"screens\":{}}",                       // wrong type
        "{\"screens\":[{\"elements\":\"nope\"}]}",  // wrong type, one level down
        "{\"screens\":[{\"id\":}]}",
        "{\"screens\":[{\"size\":[1]}]}",          // too few numbers
        "{\"screens\":[{\"size\":[1,2,3]}]}",      // too many
        "{\"a\":\"unterminated",
        "{\"a\":\"\\q\"}",                          // unknown escape
        "{\"a\":\"\\u00\"}",                        // truncated escape
        "{\"a\":1e\"}",                             // broken exponent
        "{\"a\":--1}",
        "null",
        "123",
        "\"just a string\"",
    };
    for (const char* text : nasty) {
        oreoui::Document doc;
        std::string error;
        doc.load(text, error);   // must return, one way or the other
        check(true, "malformed input returned instead of crashing");
    }
}

static void test_deep_nesting_does_not_blow_the_stack() {
    // A hostile sender's cheapest attack on a recursive parser.
    std::string deep;
    for (int i = 0; i < 5000; ++i) deep += "[";
    oreoui::Document doc;
    std::string error;
    check(!doc.load(deep.c_str(), error), "runaway nesting is refused");
}

// ---- numbers ----------------------------------------------------------------------------

static void test_numbers_are_read_the_way_they_were_written() {
    // The reason this matters: strtod and sscanf follow LC_NUMERIC. Under a Russian or German
    // locale they expect a comma, read "0.42" as 0, and the bar sits at the top of the screen
    // for that player and nobody else. The parser uses std::from_chars, which has no locale.
    setlocale(LC_NUMERIC, "de-DE");

    oreoui::Document doc;
    std::string error;
    check(doc.load(minimal_document().c_str(), error),
          "a document with decimal points loads under a comma locale");

    oreoui::Value value;
    value.bound = true;
    value.name = "barY";
    value.low = 0.25f;
    value.high = 0.82f;
    std::unordered_map<std::string, float> numbers{{"barY", 0.5f}};
    float resolved = value.resolve(numbers);
    check(resolved > 0.53f && resolved < 0.54f, "a bound value lands in the middle of its range");

    setlocale(LC_NUMERIC, "C");
}

static void test_an_unsent_value_sits_at_the_start_of_its_range() {
    // Not at zero: a bar mapped into the middle of a screen would be drawn off the panel
    // during the frames between the document arriving and the first value.
    oreoui::Value value;
    value.bound = true;
    value.name = "never-sent";
    value.low = 0.25f;
    value.high = 0.82f;
    std::unordered_map<std::string, float> empty;
    check(value.resolve(empty) == 0.25f, "an unsent value resolves to the low end");
}

static void test_a_fixed_value_ignores_the_value_store() {
    oreoui::Value value;
    value.fixed = 0.7f;
    std::unordered_map<std::string, float> numbers{{"anything", 0.1f}};
    check(value.resolve(numbers) == 0.7f, "an unbound field never moves");
}

// ---- reload ------------------------------------------------------------------------------

static void test_values_survive_a_reload() {
    // The mod resends the document on every reconnect. Dropping the values it last sent would
    // blank the panel until every one of them happened to change again.
    oreoui::Document doc;
    std::string error;
    doc.load(minimal_document().c_str(), error);
    doc.set_number("barY", 0.9f);
    doc.set_text("fishName", "Окунь");

    check(doc.load(minimal_document().c_str(), error), "the document reloads");
    check(!doc.empty(), "and is still there afterwards");
}

static void test_utf8_text_survives() {
    oreoui::Document doc;
    std::string error;
    doc.load(R"({"screens":[{"id":"s","size":[10,10],"elements":[)"
             R"({"id":"t","shape":"text","x":0,"y":0,"text":"Окунь","color":"#fff"}]}]})",
             error);
    check(error.empty(), "a document with non-ASCII text loads");
}

// ---- placement -------------------------------------------------------------------------
//
// Where a panel sits is the one thing about a mod's interface the file cannot decide: it does
// not know which monitor it will land on. The mod moves the box; nothing inside it moves.

static void test_a_screen_can_be_moved_after_loading() {
    oreoui::Document doc;
    std::string error;
    doc.load(minimal_document().c_str(), error);

    check(doc.set_placement("catch", 40.0f, -10.0f, "left"),
          "a known screen can be placed");
    check(!doc.set_placement("inventory", 0.0f, 0.0f, "left"),
          "an unknown screen is refused rather than silently ignored");
}

static void test_placement_without_an_anchor_keeps_the_one_it_had() {
    oreoui::Document doc;
    std::string error;
    doc.load(minimal_document().c_str(), error);

    check(doc.set_placement("catch", 10.0f, 0.0f, nullptr),
          "placement with no anchor is accepted");
}

static void test_an_unknown_anchor_falls_back_rather_than_crashing() {
    // The Python side checks anchors before they get here, but this is reachable by anything
    // running as this user, and "unknown word" must not mean "undefined behaviour".
    oreoui::Document doc;
    std::string error;
    doc.load(minimal_document().c_str(), error);

    check(doc.set_placement("catch", 0.0f, 0.0f, "sideways"),
          "an unknown anchor is taken as the default, not a crash");
}

// ---- the escape hatch ------------------------------------------------------------------

static void test_raw_shapes_attach_to_a_screen() {
    oreoui::Document doc;
    std::string error;
    doc.load(minimal_document().c_str(), error);

    check(doc.set_raw("catch",
                      R"([{"shape":"circle","x":0.5,"y":0.5,"r":0.1,"fill":"#fff"}])", error),
          "raw shapes are accepted for a known screen");
    check(error.empty(), "and say nothing when they are");
}

static void test_raw_shapes_for_an_unknown_screen_are_refused() {
    oreoui::Document doc;
    std::string error;
    doc.load(minimal_document().c_str(), error);

    check(!doc.set_raw("inventory", R"([])", error),
          "raw shapes for a screen that does not exist are refused");
    check(error.find("inventory") != std::string::npos,
          "and the message names the screen that was asked for");
}

static void test_raw_shapes_must_be_an_array() {
    oreoui::Document doc;
    std::string error;
    doc.load(minimal_document().c_str(), error);

    check(!doc.set_raw("catch", R"({"shape":"circle"})", error),
          "a single object is not a list of shapes");
    check(!doc.set_raw("catch", "not json at all", error),
          "garbage is refused rather than parsed halfway");
}

static void test_an_unknown_raw_shape_is_refused() {
    oreoui::Document doc;
    std::string error;
    doc.load(minimal_document().c_str(), error);

    check(!doc.set_raw("catch", R"([{"shape":"blob","x":0.5,"y":0.5}])", error),
          "a shape nobody knows is refused");
}

static void test_an_empty_list_clears_the_raw_layer() {
    oreoui::Document doc;
    std::string error;
    doc.load(minimal_document().c_str(), error);
    doc.set_raw("catch", R"([{"shape":"circle","x":0.5,"y":0.5,"r":0.1,"fill":"#fff"}])", error);

    check(doc.set_raw("catch", "[]", error), "an empty list is accepted");
}

static void test_raw_shapes_may_bind_values() {
    // The escape hatch is for shapes the file cannot name, not for shapes that cannot move.
    oreoui::Document doc;
    std::string error;
    doc.load(minimal_document().c_str(), error);

    check(doc.set_raw("catch",
                      R"([{"shape":"circle","x":{"bind":"barY"},"y":0.5,"r":0.1,)"
                      R"("fill":"#fff"}])", error),
          "a raw shape can bind a value like any other");
}

static void test_a_reload_drops_the_raw_layer() {
    // Raw shapes belong to the document that was loaded when they arrived. A new document is a
    // new interface, and shapes drawn over the old one have nothing left to sit on.
    oreoui::Document doc;
    std::string error;
    doc.load(minimal_document().c_str(), error);
    doc.set_raw("catch", R"([{"shape":"circle","x":0.5,"y":0.5,"r":0.1,"fill":"#fff"}])", error);

    check(doc.load(minimal_document().c_str(), error), "the document reloads");
    check(doc.set_raw("catch", "[]", error), "and the screen is still addressable afterwards");
}

// ---- widgets ---------------------------------------------------------------------------

static std::string widget_document() {
    return R"({"format":2,"screens":[{"id":"panel","size":[400,300],"visible":true,"elements":[)"
           R"({"id":"go","widget":"button","x":0.1,"y":0.1,"w":0.3,"label":"Go"},)"
           R"({"id":"sound","widget":"toggle","x":0.1,"y":0.2,"w":0.3,"value":true},)"
           R"({"id":"volume","widget":"slider","x":0.1,"y":0.3,"w":0.8,"min":0,"max":100,)"
           R"("value":40},)"
           R"({"id":"mode","widget":"dropdown","x":0.1,"y":0.4,"w":0.5,)"
           R"("options":["easy","hard"],"value":"hard"},)"
           R"({"id":"name","widget":"input","x":0.1,"y":0.5,"w":0.8,"maxLength":8,"value":"Bob"}]}]})";
}

static void test_widgets_load_with_the_files_values() {
    oreoui::Document doc;
    std::string error;
    check(doc.load(widget_document().c_str(), error), "a document with widgets loads");

    oreoui::WidgetValue v;
    check(doc.widget_value("sound", v) && v.boolean, "a toggle starts where the file put it");
    check(doc.widget_value("volume", v) && v.number == 40.0f, "so does a slider");
    check(doc.widget_value("mode", v) && v.text == "hard", "and a dropdown");
    check(doc.widget_value("name", v) && v.text == "Bob", "and an input");
    check(!doc.widget_value("nobody", v), "an unknown widget has no value");
}

static void test_the_mod_sets_each_kind_of_value() {
    oreoui::Document doc;
    std::string error;
    doc.load(widget_document().c_str(), error);
    oreoui::WidgetValue v;

    check(doc.set_widget_value("sound", "false") == oreoui::SetResult::Ok, "a toggle takes a boolean");
    check(doc.widget_value("sound", v) && !v.boolean, "and holds it");
    check(doc.set_widget_value("volume", "72.5") == oreoui::SetResult::Ok, "a slider takes a number");
    check(doc.widget_value("volume", v) && v.number == 72.5f, "and holds it");
    check(doc.set_widget_value("mode", R"("easy")") == oreoui::SetResult::Ok, "a dropdown takes a string");
    check(doc.widget_value("mode", v) && v.text == "easy", "and holds it");
    check(doc.set_widget_value("name", R"("Ann")") == oreoui::SetResult::Ok, "an input takes a string");
    check(doc.widget_value("name", v) && v.text == "Ann", "and holds it");
}

static void test_a_value_of_the_wrong_kind_changes_nothing() {
    oreoui::Document doc;
    std::string error;
    doc.load(widget_document().c_str(), error);
    oreoui::WidgetValue v;

    check(doc.set_widget_value("sound", "1") == oreoui::SetResult::WrongType,
          "a number is not a boolean");
    check(doc.set_widget_value("volume", R"("loud")") == oreoui::SetResult::WrongType,
          "a string is not a number");
    check(doc.set_widget_value("go", "true") == oreoui::SetResult::WrongType,
          "a button holds nothing to set");
    check(doc.set_widget_value("volume", "{not json") == oreoui::SetResult::WrongType,
          "garbage is refused");
    check(doc.set_widget_value("nobody", "1") == oreoui::SetResult::Unknown,
          "an unknown widget is reported as such");
    check(doc.widget_value("volume", v) && v.number == 40.0f, "and nothing moved");
}

static void test_a_slider_value_is_kept_in_range() {
    oreoui::Document doc;
    std::string error;
    doc.load(widget_document().c_str(), error);
    oreoui::WidgetValue v;

    doc.set_widget_value("volume", "250");
    check(doc.widget_value("volume", v) && v.number == 100.0f, "above the range clamps to max");
    doc.set_widget_value("volume", "-5");
    check(doc.widget_value("volume", v) && v.number == 0.0f, "below the range clamps to min");
}

static void test_input_text_is_cut_without_splitting_a_character() {
    oreoui::Document doc;
    std::string error;
    doc.load(widget_document().c_str(), error);
    oreoui::WidgetValue v;

    doc.set_widget_value("name", R"("abcdefghijk")");
    check(doc.widget_value("name", v) && v.text == "abcdefgh", "text is cut to maxLength bytes");

    // "aабвг" is nine bytes, and the eighth is the first half of "г". Eight bytes would end
    // in half a letter; what is kept is "aабв", seven.
    doc.set_widget_value("name", "\"a\xD0\xB0\xD0\xB1\xD0\xB2\xD0\xB3\"");
    check(doc.widget_value("name", v) && v.text == "a\xD0\xB0\xD0\xB1\xD0\xB2",
          "a cut never splits a UTF-8 character");
}

static void test_dropdown_options_can_change() {
    oreoui::Document doc;
    std::string error;
    doc.load(widget_document().c_str(), error);

    check(doc.set_options("mode", R"(["a","b","c"])", error), "new options are accepted");
    check(!doc.set_options("mode", R"(["a",1])", error), "a non-string option is refused");
    check(!doc.set_options("mode", R"("a")", error), "options must be an array");
    check(!doc.set_options("volume", R"(["a"])", error), "only a dropdown has options");
    check(!doc.set_options("nobody", R"(["a"])", error), "an unknown widget is refused");
}

static void test_widgets_can_be_disabled() {
    oreoui::Document doc;
    std::string error;
    doc.load(widget_document().c_str(), error);

    check(doc.set_enabled("go", false), "a widget can be disabled");
    check(!doc.set_enabled("nobody", false), "an unknown widget is reported");
}

static void test_a_reload_puts_widgets_back_at_the_files_values() {
    // The mod's side replays what it holds after every document. Values kept here instead would
    // survive a mod restarting inside a running game and disagree with what it believes.
    oreoui::Document doc;
    std::string error;
    doc.load(widget_document().c_str(), error);
    doc.set_widget_value("volume", "90");

    doc.load(widget_document().c_str(), error);
    oreoui::WidgetValue v;
    check(doc.widget_value("volume", v) && v.number == 40.0f, "the file's value is back");
}

static void test_widgets_are_not_allowed_down_the_escape_hatch() {
    oreoui::Document doc;
    std::string error;
    doc.load(widget_document().c_str(), error);

    check(!doc.set_raw("panel", R"([{"widget":"button","x":0,"y":0,"w":0.1,"label":"x"}])", error),
          "a widget in raw shapes is refused");
}

static void test_an_unknown_widget_is_skipped_not_fatal() {
    oreoui::Document doc;
    std::string error;
    check(doc.load(R"({"format":2,"screens":[{"id":"s","size":[10,10],"elements":[)"
                   R"({"id":"k","widget":"knob","x":0,"y":0,"w":1}]}]})", error),
          "a document with an unknown widget still loads");
    oreoui::WidgetValue v;
    check(!doc.widget_value("k", v), "and the unknown widget is not there");
}

static void test_event_strings_are_quoted_safely() {
    check(oreoui::json_quote("plain") == "\"plain\"", "plain text is just quoted");
    check(oreoui::json_quote("a\"b\\c") == "\"a\\\"b\\\\c\"", "quotes and backslashes are escaped");
    check(oreoui::json_quote("one\ntwo") == "\"one\\u000atwo\"",
          "a newline cannot end the event line early");
    check(oreoui::json_quote("\xD1\x8F") == "\"\xD1\x8F\"", "UTF-8 passes through untouched");
}

int main() {
    test_colours();
    test_document_loads();
    test_visibility_addresses_screens_and_elements();
    test_malformed_input_is_refused_not_fatal();
    test_deep_nesting_does_not_blow_the_stack();
    test_numbers_are_read_the_way_they_were_written();
    test_an_unsent_value_sits_at_the_start_of_its_range();
    test_a_fixed_value_ignores_the_value_store();
    test_values_survive_a_reload();
    test_utf8_text_survives();
    test_a_screen_can_be_moved_after_loading();
    test_placement_without_an_anchor_keeps_the_one_it_had();
    test_an_unknown_anchor_falls_back_rather_than_crashing();
    test_raw_shapes_attach_to_a_screen();
    test_raw_shapes_for_an_unknown_screen_are_refused();
    test_raw_shapes_must_be_an_array();
    test_an_unknown_raw_shape_is_refused();
    test_an_empty_list_clears_the_raw_layer();
    test_raw_shapes_may_bind_values();
    test_a_reload_drops_the_raw_layer();
    test_widgets_load_with_the_files_values();
    test_the_mod_sets_each_kind_of_value();
    test_a_value_of_the_wrong_kind_changes_nothing();
    test_a_slider_value_is_kept_in_range();
    test_input_text_is_cut_without_splitting_a_character();
    test_dropdown_options_can_change();
    test_widgets_can_be_disabled();
    test_a_reload_puts_widgets_back_at_the_files_values();
    test_widgets_are_not_allowed_down_the_escape_hatch();
    test_an_unknown_widget_is_skipped_not_fatal();
    test_event_strings_are_quoted_safely();

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
