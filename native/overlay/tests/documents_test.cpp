// Documents as the overlay runs them, in RmlUi without a GPU: loading from a mod's folder,
// values the documents bind to, and what the player's actions send back up the pipe.

#include <windows.h>

#include <crtdbg.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <RmlUi/Core.h>

#include "ipc/outbox.h"
#include "no_gpu.h"
#include "ui/documents.h"
#include "ui/file_access.h"
#include "ui/polygon_element.h"
#include "ui/rml_system.h"
#include "ui/ui_commands.h"
#include "ui/visor_filter.h"

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

static std::string g_dir;
static NoGpu g_gpu;

static void write_file(const std::string& name, const std::string& text) {
    std::ofstream(g_dir + "\\" + name, std::ios::binary) << text;
}

static std::vector<std::string> sent() {
    std::vector<std::string> lines = outbox::take();
    return lines;
}

static bool contains(const std::vector<std::string>& lines, const std::string& line) {
    for (const std::string& l : lines)
        if (l == line) return true;
    return false;
}

static void apply(ui_commands::Command command) { documents::apply(command); }

static void load(const std::string& mod, const std::string& doc, const std::string& file) {
    apply({ui_commands::Verb::Load, mod, doc, "", g_dir + "\\" + file});
}

template <typename T>
static void set(const std::string& mod, const std::string& name, T value) {
    ui_commands::Command command{ui_commands::Verb::Set, mod, "", name};
    command.value = Rml::Variant(value);
    apply(command);
}

static void show(const std::string& mod, const std::string& doc, ui_commands::ShowMode mode) {
    ui_commands::Command command{ui_commands::Verb::Show, mod, doc};
    command.mode = mode;
    apply(command);
}

static const char* kPanel = R"(<rml><head><style>
    .fish { position: absolute; width: 10px; height: 10px; }
</style></head>
<body data-model="fishing">
    <div id="fish" class="fish" data-style-top="fishY * 100 + 'px'"></div>
    <div id="name">{{ fishName }}</div>
    <button id="save"><span id="label-free">Save</span></button>
    <input id="sound" type="checkbox" data-checked="sound"/>
</body></rml>)";

static void test_a_document_binds_to_its_mods_values(Rml::Context* context) {
    apply({ui_commands::Verb::Root, "fishing", "", "", g_dir});
    set("fishing", "fishY", 0.5);
    set("fishing", "fishName", Rml::String("Pike"));
    load("fishing", "catch", "catch.rml");
    check(contains(sent(), "OK load fishing catch"), "the document loads and says so");
    show("fishing", "catch", ui_commands::ShowMode::Show);
    context->Update();

    Rml::ElementDocument* document = context->GetDocument(0);
    check(document && document->IsVisible(), "the document is showing");
    Rml::Element* fish = document ? document->GetElementById("fish") : nullptr;
    check(fish && fish->GetProperty<float>("top") == 50.0f, "fishY places the fish");
    Rml::Element* name = document ? document->GetElementById("name") : nullptr;
    check(name && name->GetInnerRML() == "Pike", "fishName fills the text");

    set("fishing", "fishY", 0.25);
    context->Update();
    check(fish && fish->GetProperty<float>("top") == 25.0f, "a new value moves the fish");
}

static void test_a_click_is_reported_by_the_nearest_id(Rml::Context* context) {
    Rml::ElementDocument* document = context->GetDocument(0);
    sent();
    if (Rml::Element* label = document->GetElementById("label-free")) {
        label->SetId("");
        label->Click();
    }
    check(contains(sent(), "EV fishing catch click save null"), "a click on the label is the button's");
}

static void test_a_bound_checkbox_tells_the_mod(Rml::Context* context) {
    set("fishing", "sound", true);
    context->Update();
    sent();
    Rml::Element* sound = context->GetDocument(0)->GetElementById("sound");
    if (sound) sound->Click();
    context->Update();
    std::vector<std::string> lines = sent();
    check(contains(lines, "VAR fishing sound false"), "unticking sends the new value");
    check(contains(lines, "EV fishing catch change sound false"), "and the change, typed");
}

static void test_a_file_outside_the_folder_is_refused(Rml::Context*) {
    apply({ui_commands::Verb::Load, "fishing", "evil", "", "C:\\Windows\\win.ini"});
    std::vector<std::string> lines = sent();
    check(!lines.empty() && lines[0].rfind("ERR load fishing evil", 0) == 0,
          "a document outside the mod's folder is refused");
}

static void test_a_broken_document_says_so(Rml::Context*) {
    write_file("missing_image.rml",
               "<rml><body data-model='fishing'><img src='nope.png'/></body></rml>");
    load("fishing", "broken", "missing_image.rml");
    check(contains(sent(), "OK load fishing broken"), "a document with a missing image still loads");
    load("fishing", "gone", "no_such_file.rml");
    std::vector<std::string> lines = sent();
    bool refused = false;
    for (const std::string& line : lines) refused |= line.rfind("ERR load fishing gone", 0) == 0;
    check(refused, "a file that is not there is an error");
}

static void test_two_mods_keep_their_values_apart(Rml::Context* context) {
    write_file("other.rml", "<rml><head/><body data-model='hunting'><div id='name'>{{ fishName }}"
                            "</div></body></rml>");
    set("hunting", "fishName", Rml::String("Deer"));
    load("hunting", "panel", "other.rml");
    show("hunting", "panel", ui_commands::ShowMode::Show);
    context->Update();
    Rml::ElementDocument* hunting = nullptr;
    for (int i = 0; i < context->GetNumDocuments(); ++i)
        if (context->GetDocument(i)->GetSourceURL().find("other.rml") != Rml::String::npos)
            hunting = context->GetDocument(i);
    Rml::Element* name = hunting ? hunting->GetElementById("name") : nullptr;
    check(name && name->GetInnerRML() == "Deer", "the same name means different values per mod");
}

static void test_modal_takes_the_input_and_disconnect_closes_all(Rml::Context* context) {
    check(!documents::takes_input(), "a plain document does not take the input");
    show("fishing", "catch", ui_commands::ShowMode::Modal);
    check(documents::takes_input(), "a modal one does");
    apply({ui_commands::Verb::Disconnected});
    context->Update();
    check(!documents::takes_input() && documents::visible_count() == 0,
          "a disconnect closes every mod's documents");
}

static void test_a_reload_keeps_the_way_it_was_shown(Rml::Context* context) {
    load("fishing", "catch", "catch.rml");
    show("fishing", "catch", ui_commands::ShowMode::Show);
    load("fishing", "catch", "catch.rml");
    context->Update();
    check(documents::visible_count() == 1, "a document loaded again is still showing");
}

static void test_the_visor_filter_is_known(Rml::Context* context) {
    write_file("visor.rml", "<rml><head><style>div { filter: visor(0.2 0.05); }</style></head>"
                            "<body data-model='fishing'><div id='hud'/></body></rml>");
    rml_system::forget_repeats();
    load("fishing", "visor", "visor.rml");
    context->Update();
    bool complained = false;
    for (const std::string& line : sent()) {
        if (line.rfind("LOG ", 0) == 0 && line.find("filter") != std::string::npos) {
            std::printf("  %s\n", line.c_str());
            complained = true;
        }
    }
    check(!complained, "filter: visor(...) parses without a warning");
}

// The Runtime's own assets, straight from the source tree.
static void test_the_runtime_font_and_styles_load(Rml::Context* context) {
    std::string assets = OREO_ASSETS_DIR;
    file_access::allow(assets);
    check(Rml::LoadFontFace(assets + "/fonts/gamefont.ttf", true), "the game font loads");
    sent();
    documents::attach(context, assets + "/ui/base.rcss");
    bool complained = false;
    for (const std::string& line : sent()) {
        std::printf("  %s\n", line.c_str());
        complained |= line.rfind("LOG ", 0) == 0;
    }
    check(!complained, "the base style sheet reads without a warning");
}

static void test_text_has_a_width(Rml::Context* context) {
    write_file("text.rml", "<rml><head/><body data-model='fishing'><span id='t'>"
                           "\xD0\xA9\xD1\x83\xD0\xBA\xD0\xB0 and pike</span></body></rml>");
    load("fishing", "text", "text.rml");
    show("fishing", "text", ui_commands::ShowMode::Show);
    context->Update();
    Rml::Element* text = nullptr;
    for (int i = 0; i < context->GetNumDocuments() && !text; ++i)
        text = context->GetDocument(i)->GetElementById("t");
    check(text && text->GetClientWidth() > 50.0f, "text in the game font takes up room");
    bool fontWarning = false;
    for (const std::string& line : sent()) {
        if (line.find("font") == std::string::npos) continue;
        std::printf("  %s\n", line.c_str());
        fontWarning = true;
    }
    check(!fontWarning, "no warning about fonts");
}

// The demo that ships with the Runtime must load clean: it is what a modder copies first.
static void test_the_widgets_demo_loads_without_a_warning(Rml::Context* context) {
    std::string demo = OREO_EXAMPLES_DIR "/widgets_demo";
    apply({ui_commands::Verb::Root, "widgets_demo", "", "", demo});
    for (const char* name : {"device", "mode", "name", "clicks", "last"})
        set("widgets_demo", name, Rml::String(""));
    set("widgets_demo", "sound", true);
    set("widgets_demo", "volume", 40.0);
    sent();
    apply({ui_commands::Verb::Load, "widgets_demo", "demo", "", demo + "/ui/demo.rml"});
    show("widgets_demo", "demo", ui_commands::ShowMode::Modal);
    context->Update();
    context->Render();
    bool clean = true;
    for (const std::string& line : sent()) {
        if (line.rfind("OK ", 0) == 0 || line.rfind("VAR ", 0) == 0) continue;
        std::printf("  %s\n", line.c_str());
        clean = false;
    }
    check(clean, "the widgets demo loads, lays out and renders without a complaint");
}

// <img src='../images/fish.png'> in mod/ui/catch.rml is mod/images/fish.png, not a path from
// nowhere.
static void test_an_image_is_found_next_to_its_document(Rml::Context* context) {
    CreateDirectoryA((g_dir + "\\ui").c_str(), nullptr);
    write_file("ui\\pic.rml", "<rml><head/><body data-model='fishing'><img src='../images/fish.png'/>"
                              "</body></rml>");
    g_gpu.images.clear();
    load("fishing", "pic", "ui\\pic.rml");
    show("fishing", "pic", ui_commands::ShowMode::Show);
    context->Update();
    context->Render();
    std::string wanted = g_dir + "\\images\\fish.png";
    bool found = false;
    for (const Rml::String& source : g_gpu.images) {
        std::printf("  asked for %s\n", source.c_str());
        found |= file_access::canonical(source) == file_access::canonical(wanted);
    }
    check(found, "the image is looked for beside the document's folder");
}

// Inline SVG, as a design tool exports it, becomes a texture the size of its element.
static void test_inline_svg_is_drawn_at_its_size(Rml::Context* context) {
    write_file("svg.rml",
               "<rml><head/><body data-model='fishing'>"
               "<svg width='128' height='64' viewBox='0 0 128 64'>"
               "<path d='M24 36 L32 26 H96 L104 36' fill='none' stroke='rgba(255,255,255,0.8)' "
               "stroke-width='2'/></svg></body></rml>");
    g_gpu.generated.clear();
    sent();
    load("fishing", "svg", "svg.rml");
    show("fishing", "svg", ui_commands::ShowMode::Show);
    context->Update();
    context->Render();
    bool drawn = false;
    for (const Rml::Vector2i& size : g_gpu.generated) drawn |= size.x == 128 && size.y == 64;
    check(drawn, "the SVG is rendered into a 128x64 texture");
    bool complained = false;
    for (const std::string& line : sent()) {
        if (line.rfind("LOG ", 0) != 0) continue;
        std::printf("  %s\n", line.c_str());
        complained = true;
    }
    check(!complained, "and RmlUi has nothing to say about it");
}

// A failure prints and exits; it never opens a window on somebody's screen.
static void quiet_failures() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    for (int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        (void)type;   // the calls below vanish in a release build
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
}

int main() {
    quiet_failures();
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    g_dir = std::string(temp) + "oreo_documents_test";
    CreateDirectoryA(g_dir.c_str(), nullptr);
    write_file("catch.rml", kPanel);


    Rml::SetRenderInterface(&g_gpu);
    Rml::SetSystemInterface(rml_system::get());
    Rml::SetFileInterface(file_access::get());
    if (!Rml::Initialise()) {
        std::printf("RmlUi did not start\n");
        return 1;
    }
    visor_filter::register_with_rmlui();
    polygon_element::register_with_rmlui();
    Rml::Context* context = Rml::CreateContext("test", {1000, 1000});
    test_the_runtime_font_and_styles_load(context);

    test_a_document_binds_to_its_mods_values(context);
    test_a_click_is_reported_by_the_nearest_id(context);
    test_a_bound_checkbox_tells_the_mod(context);
    test_a_file_outside_the_folder_is_refused(context);
    test_a_broken_document_says_so(context);
    test_two_mods_keep_their_values_apart(context);
    test_modal_takes_the_input_and_disconnect_closes_all(context);
    test_a_reload_keeps_the_way_it_was_shown(context);
    test_the_visor_filter_is_known(context);
    test_text_has_a_width(context);
    test_the_widgets_demo_loads_without_a_warning(context);
    test_an_image_is_found_next_to_its_document(context);
    test_inline_svg_is_drawn_at_its_size(context);

    Rml::Shutdown();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
