// Tests for the half of the overlay that reads what arrives over the pipe: the messages, the
// values in them, the pad, and which files a mod may make RmlUi read.

#include <windows.h>

#include <clocale>
#include <cstdio>
#include <string>

#include "input/pad_input.h"
#include "ipc/json_scalar.h"
#include "ipc/protocol.h"
#include "ui/file_access.h"
#include "ui/rml_source.h"
#include "ui/ui_commands.h"

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

static Rml::Variant parsed(const char* text, bool* ok = nullptr) {
    Rml::Variant value;
    bool result = json_scalar::parse(text, value);
    if (ok) *ok = result;
    return value;
}

static bool refused(const char* text) {
    bool ok = true;
    parsed(text, &ok);
    return !ok;
}

// The one command a line queued, or a Disconnected one when it queued none or several.
static ui_commands::Command queued(const char* line, std::string* reply = nullptr) {
    ui_commands::take();
    std::string answer = protocol::handle(line);
    if (reply) *reply = answer;
    std::vector<ui_commands::Command> commands = ui_commands::take();
    if (commands.size() != 1) return {ui_commands::Verb::Disconnected};
    return commands[0];
}

static bool rejected(const char* line) {
    std::string reply;
    ui_commands::Command command = queued(line, &reply);
    return command.verb == ui_commands::Verb::Disconnected && reply.rfind("ERR ", 0) == 0;
}

// ---- values ----------------------------------------------------------------------------

static void test_booleans_numbers_and_strings_parse() {
    check(parsed("true").Get<bool>(), "true is true");
    check(!parsed("false").Get<bool>(true), "false is false");
    check(parsed("0.42").Get<double>() == 0.42, "a fraction parses");
    check(parsed("-3").Get<double>() == -3.0, "a negative number parses");
    check(parsed("1e3").Get<double>() == 1000.0, "an exponent parses");
    check(parsed("\"fish\"").Get<Rml::String>() == "fish", "a string parses");
    check(parsed("\"\"").Get<Rml::String>().empty(), "an empty string parses");
}

static void test_numbers_ignore_the_locale() {
    std::setlocale(LC_ALL, "Russian");
    check(parsed("0.42").Get<double>() == 0.42, "0.42 is 0.42 on a Russian machine too");
    std::setlocale(LC_ALL, "C");
}

static void test_string_escapes_become_their_characters() {
    check(parsed(R"("a\"b")").Get<Rml::String>() == "a\"b", "an escaped quote");
    check(parsed(R"("a\\b")").Get<Rml::String>() == "a\\b", "an escaped backslash");
    check(parsed(R"("a\nb")").Get<Rml::String>() == "a\nb", "an escaped newline");
    check(parsed(R"("\u0444")").Get<Rml::String>() == "\xD1\x84", "a Cyrillic letter by code");
    check(parsed(R"("\ud83d\udc1f")").Get<Rml::String>() == "\xF0\x9F\x90\x9F",
          "a surrogate pair becomes one emoji");
    check(parsed("\"\xD1\x80\xD1\x8B\xD0\xB1\xD0\xB0\"").Get<Rml::String>() ==
              "\xD1\x80\xD1\x8B\xD0\xB1\xD0\xB0",
          "UTF-8 passes through untouched");
}

static void test_broken_values_are_refused() {
    check(refused(""), "nothing is not a value");
    check(refused("tru"), "a misspelt boolean");
    check(refused("0.42x"), "a number with junk after it");
    check(refused("nan"), "NaN");
    check(refused("inf"), "infinity");
    check(refused("1e999"), "a number too large to hold");
    check(refused("\"open"), "an unterminated string");
    check(refused("\"a\" b"), "junk after a string");
    check(refused(R"("\x")"), "an unknown escape");
    check(refused(R"("\ud83d")"), "half a surrogate pair");
    check(refused(R"("\udc1f")"), "a lone low surrogate");
    check(refused("\"tab\there\""), "a raw control character in a string");
    check(refused("[1]"), "an array");
    check(refused("{}"), "an object");
}

static void test_values_are_written_back_as_json() {
    check(json_scalar::write(Rml::Variant(true)) == "true", "a boolean");
    check(json_scalar::write(Rml::Variant(0.5)) == "0.5", "a number, shortest form");
    check(json_scalar::write(Rml::Variant(Rml::String("say \"hi\"\n"))) == R"("say \"hi\"\n")",
          "a string, escaped");
    check(json_scalar::quote(std::string("\x01")) == R"("\u0001")",
          "a control character as \\u");
}

// ---- messages --------------------------------------------------------------------------

static void test_each_message_queues_its_command() {
    ui_commands::Command root = queued("ROOT fishing C:\\Mods\\Fishing Minigame");
    check(root.verb == ui_commands::Verb::Root && root.mod == "fishing", "ROOT names the mod");
    check(root.path == "C:\\Mods\\Fishing Minigame", "ROOT keeps a path with spaces whole");

    ui_commands::Command load = queued("LOAD fishing catch C:\\Mods\\ui files\\catch.rml");
    check(load.verb == ui_commands::Verb::Load && load.doc == "catch", "LOAD names the document");
    check(load.path == "C:\\Mods\\ui files\\catch.rml", "LOAD keeps a path with spaces whole");

    check(queued("SHOW fishing catch modal").mode == ui_commands::ShowMode::Modal, "SHOW modal");
    check(queued("SHOW fishing catch show").mode == ui_commands::ShowMode::Show, "SHOW show");
    check(queued("SHOW fishing catch hide").mode == ui_commands::ShowMode::Hide, "SHOW hide");
    check(queued("CLOSE fishing catch").verb == ui_commands::Verb::Close, "CLOSE");

    ui_commands::Command set = queued("SET fishing fishY 0.42");
    check(set.verb == ui_commands::Verb::Set && set.name == "fishY", "SET names the value");
    check(set.value.Get<double>() == 0.42, "SET carries the value");
    check(queued("SET fishing title \"a b c\"").value.Get<Rml::String>() == "a b c",
          "a string value may hold spaces");
}

static void test_malformed_messages_are_refused_out_loud() {
    check(rejected("ROOT fishing"), "ROOT without a folder");
    check(rejected("LOAD fishing catch"), "LOAD without a path");
    check(rejected("SHOW fishing catch sideways"), "SHOW with an unknown mode");
    check(rejected("SHOW fishing"), "SHOW without a document");
    check(rejected("CLOSE fishing catch extra"), "CLOSE with something after it");
    check(rejected("SET fishing fishY"), "SET without a value");
    check(rejected("SET fishing fishY 0,42"), "SET with a value that is not JSON");
}

static void test_names_are_what_rmlui_accepts() {
    check(rejected("SET 1fish fishY 1"), "a mod may not start with a digit");
    check(rejected("SET fi-sh fishY 1"), "a mod may not hold a dash");
    check(rejected("SET oreo_diagnostics cpu 1"), "oreo_ belongs to the Runtime");
    check(rejected("SET fishing fish.y 1"), "a value name may not hold a dot");
    check(rejected("SET fishing size 1"), "a value may not take a word RmlUi keeps");
    check(rejected("SET fishing IT 1"), "...in any case");
    check(rejected("LOAD fishing ../catch C:\\x.rml"), "a document id may not hold a path");
    std::string longName(65, 'a');
    check(rejected(("SET " + longName + " fishY 1").c_str()), "a mod name of 65 characters");
    check(queued("SHOW fishing settings-2 show").doc == "settings-2", "a document id may hold dashes");
}

static void test_unknown_messages_are_ignored() {
    check(queued("UI {}").verb == ui_commands::Verb::Disconnected, "the old UI message does nothing");
    check(queued("").verb == ui_commands::Verb::Disconnected, "an empty line does nothing");
}

static void test_pad_buttons_are_read_by_name() {
    protocol::handle("PAD a,up");
    check(pad_input::held() == (pad_input::A | pad_input::Up), "a and up are held");
    protocol::handle("PAD");
    check(pad_input::held() == 0, "an empty PAD lets go of everything");
    protocol::handle("PAD start, banana ,b");
    check(pad_input::held() == (pad_input::Start | pad_input::B), "unknown names are skipped");
    protocol::disconnected();
    check(pad_input::held() == 0, "a disconnect lets go of everything");
}

// ---- files -----------------------------------------------------------------------------

static void test_a_mod_reads_only_below_its_folder() {
    file_access::allow("C:\\Mods\\Fishing");
    check(file_access::allowed("C:\\Mods\\Fishing\\ui\\catch.rml"), "a file in the folder");
    check(file_access::allowed("c:/mods/fishing/UI/catch.rml"), "...whatever the case and slashes");
    check(!file_access::allowed("C:\\Mods\\Fishing\\..\\Other\\secret.txt"), "not via ..");
    check(!file_access::allowed("C:\\Mods\\Fishing2\\ui.rml"), "not a sibling sharing the prefix");
    check(!file_access::allowed("C:\\Windows\\win.ini"), "not anywhere else");
    check(!file_access::allowed("ui\\catch.rml"), "not a relative path from the game's folder");
    check(!file_access::allowed(""), "not nothing");
}

// ---- the base style sheet ----------------------------------------------------------------

static std::string linked(std::string rml) {
    return rml_source::link_base_sheet(rml, "C:\\base.rcss") ? rml : "refused";
}

static void test_the_base_sheet_is_linked_ahead_of_the_documents_own() {
    const std::string link = "<link type=\"text/rcss\" href=\"C:\\base.rcss\"/>";
    check(linked("<rml><head><link href='a.rcss'/></head></rml>") ==
              "<rml><head>" + link + "<link href='a.rcss'/></head></rml>",
          "first in an existing head");
    check(linked("<RML><HEAD title='x'></HEAD></RML>") ==
              "<RML><HEAD title='x'>" + link + "</HEAD></RML>",
          "whatever the case, attributes and all");
    check(linked("<rml><head/><body/></rml>") == "<rml><head>" + link + "</head><body/></rml>",
          "an empty <head/> is opened up");
    check(linked("<rml>\n<body><header/></body></rml>") ==
              "<rml><head>" + link + "</head>\n<body><header/></body></rml>",
          "a document without a head gets one; <header> is not a head");
    check(linked("<html><body/></html>") == "refused", "a file that is not RML is refused");
}

int main() {
    test_booleans_numbers_and_strings_parse();
    test_numbers_ignore_the_locale();
    test_string_escapes_become_their_characters();
    test_broken_values_are_refused();
    test_values_are_written_back_as_json();
    test_each_message_queues_its_command();
    test_malformed_messages_are_refused_out_loud();
    test_names_are_what_rmlui_accepts();
    test_unknown_messages_are_ignored();
    test_pad_buttons_are_read_by_name();
    test_a_mod_reads_only_below_its_folder();
    test_the_base_sheet_is_linked_ahead_of_the_documents_own();

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
