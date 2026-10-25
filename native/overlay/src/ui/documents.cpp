#include "ui/documents.h"

#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/StringUtilities.h>

#include "ipc/json_scalar.h"
#include "ipc/outbox.h"
#include "log.h"
#include "ui/element_events.h"
#include "ui/file_access.h"
#include "ui/rml_source.h"
#include "ui/rml_system.h"

namespace documents {

namespace {

using ui_commands::Command;
using ui_commands::ShowMode;
using ui_commands::Verb;

struct Document {
    Rml::ElementDocument* element = nullptr;
    ShowMode mode = ShowMode::Hide;
};

struct Mod {
    std::string name;
    Rml::DataModelConstructor model;
    Rml::DataModelHandle handle;
    std::unordered_map<std::string, Rml::Variant> values;
    std::map<std::string, Document> documents;
};

struct Registry {
    Rml::Context* context = nullptr;
    std::string baseSheet;
    // Kept across disconnects: RmlUi cannot rebind a name, and the mod sends its values again.
    std::map<std::string, std::unique_ptr<Mod>> mods;
};
Registry g_registry;

void reply(const std::string& line) {
    std::vector<std::string> lines = {line};
    outbox::post(lines);
}

Mod* mod_named(const std::string& name) {
    auto it = g_registry.mods.find(name);
    if (it != g_registry.mods.end()) return it->second.get();
    auto mod = std::make_unique<Mod>();
    mod->name = name;
    mod->model = g_registry.context->CreateDataModel(name, nullptr, true);
    if (!mod->model) {
        LOG_ERROR("ui: could not create the data model for %s.", name.c_str());
        return nullptr;
    }
    mod->handle = mod->model.GetModelHandle();
    return (g_registry.mods[name] = std::move(mod)).get();
}

// A range hands its value over as text; a value the mod made a number stays one.
Rml::Variant keep_kind(const Rml::Variant& old, const Rml::Variant& in) {
    bool wasNumber = old.GetType() != Rml::Variant::NONE && old.GetType() != Rml::Variant::BOOL &&
                     old.GetType() != Rml::Variant::STRING;
    Rml::Variant number;
    if (wasNumber && in.GetType() == Rml::Variant::STRING &&
        json_scalar::parse(in.Get<Rml::String>(), number) && number.GetType() != Rml::Variant::STRING)
        return number;
    return in;
}

// Through data-value, the player can change a value too; the mod hears of it.
void bind(Mod& mod, const std::string& name) {
    Mod* owner = &mod;
    mod.model.BindFunc(
        name, [owner, name](Rml::Variant& out) { out = owner->values[name]; },
        [owner, name](const Rml::Variant& in) {
            Rml::Variant& value = owner->values[name];
            Rml::Variant changed = keep_kind(value, in);
            if (changed == value) return;   // a control settling on the value it was given
            value = changed;
            reply("VAR " + owner->name + " " + name + " " + json_scalar::write(value));
        });
}

void show(Document& document, ShowMode mode) {
    document.mode = mode;
    if (mode == ShowMode::Hide) document.element->Hide();
    else if (mode == ShowMode::Modal) document.element->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::Auto);
    else document.element->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
}

void close(Document& document) {
    if (document.element) document.element->Close();
    document.element = nullptr;
}

void load(Mod& mod, const Command& command) {
    std::string where = mod.name + " " + command.doc;
    if (!file_access::allowed(command.path)) {
        reply("ERR load " + where + " " + command.path + " is outside the mod's folder");
        return;
    }
    std::string path = file_access::canonical(command.path);
    std::string rml;
    if (!file_access::get()->LoadFile(path, rml)) {
        reply("ERR load " + where + " " + command.path + " could not be read");
        return;
    }
    if (!rml_source::link_base_sheet(rml, g_registry.baseSheet)) {
        reply("ERR load " + where + " " + command.path + " has no <rml> element");
        return;
    }
    rml_system::forget_repeats();
    Document& document = mod.documents[command.doc];
    ShowMode mode = document.mode;
    close(document);
    // The document's own path, which relative links resolve against, as a URL RmlUi can parse:
    // forward slashes, and the drive letter's colon hidden as RmlUi's file streams hide it.
    std::string url = Rml::StringUtilities::Replace(Rml::StringUtilities::Replace(path, '\\', '/'),
                                                    ':', '|');
    document.element = g_registry.context->LoadDocumentFromMemory(rml, url);
    if (!document.element) {
        mod.documents.erase(command.doc);
        reply("ERR load " + where + " " + command.path + " is not a document; see the LOG lines");
        return;
    }
    element_events::listen(document.element, mod.name, command.doc);
    if (mode != ShowMode::Hide) show(document, mode);   // reloaded the way it was
    LOG("ui: %s loaded from %s", where.c_str(), command.path.c_str());
    reply("OK load " + where);
}

Document* find(Mod& mod, const std::string& doc) {
    auto it = mod.documents.find(doc);
    return it == mod.documents.end() ? nullptr : &it->second;
}

void set_value(Mod& mod, const Command& command) {
    if (!mod.values.count(command.name)) bind(mod, command.name);
    mod.values[command.name] = command.value;
    mod.handle.DirtyVariable(command.name);
}

void close_everything() {
    for (auto& [name, mod] : g_registry.mods) {
        for (auto& [id, document] : mod->documents) close(document);
        mod->documents.clear();
    }
}

}  // namespace

void attach(Rml::Context* context, const std::string& baseSheet) {
    g_registry.context = context;
    g_registry.baseSheet = file_access::canonical(baseSheet);
}

void apply(const Command& command) {
    if (command.verb == Verb::Disconnected) {
        close_everything();
        return;
    }
    if (command.verb == Verb::Root) {
        file_access::allow(command.path);
        return;
    }
    Mod* mod = mod_named(command.mod);
    if (!mod) return;
    Document* document = nullptr;
    switch (command.verb) {
        case Verb::Load:
            load(*mod, command);
            break;
        case Verb::Show:
            if ((document = find(*mod, command.doc))) show(*document, command.mode);
            else reply("ERR show " + mod->name + " " + command.doc + " is not loaded");
            break;
        case Verb::Close:
            if ((document = find(*mod, command.doc))) close(*document);
            mod->documents.erase(command.doc);
            break;
        case Verb::Set:
            set_value(*mod, command);
            break;
        default:
            break;
    }
}

bool takes_input() {
    for (auto& [name, mod] : g_registry.mods)
        for (auto& [id, document] : mod->documents)
            if (document.mode == ShowMode::Modal) return true;
    return false;
}

int visible_count() {
    int count = 0;
    for (auto& [name, mod] : g_registry.mods)
        for (auto& [id, document] : mod->documents)
            if (document.mode != ShowMode::Hide) ++count;
    return count;
}

}  // namespace documents
