#include "ui/element_events.h"

#include <vector>

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>

#include "ipc/json_scalar.h"
#include "ipc/outbox.h"

namespace element_events {

namespace {

const char* const kEvents[] = {"click", "change", "submit"};

Rml::Element* with_id(Rml::Element* element) {
    while (element && element->GetId().empty()) element = element->GetParentNode();
    return element;
}

// Typed the way the control holds it: a checkbox is a boolean, a range a number.
std::string change_value(Rml::Element* control, const Rml::Event& event) {
    Rml::String value = event.GetParameter<Rml::String>("value", "");
    Rml::String type = control->GetAttribute<Rml::String>("type", "");
    if (control->GetTagName() == "input" && (type == "checkbox" || type == "radio"))
        return control->HasAttribute("checked") ? "true" : "false";
    Rml::Variant number;
    if (control->GetTagName() == "input" && type == "range" && json_scalar::parse(value, number))
        return json_scalar::write(number);
    return json_scalar::quote(value);
}

// A form's fields, by name.
std::string submit_value(const Rml::Event& event) {
    std::string out = "{";
    for (const auto& [name, value] : event.GetParameters()) {
        if (out.size() > 1) out += ",";
        out += json_scalar::quote(name) + ":" + json_scalar::write(value);
    }
    return out + "}";
}

class Listener final : public Rml::EventListener {
public:
    Listener(std::string mod, std::string doc) : prefix_("EV " + mod + " " + doc + " ") {}

    void ProcessEvent(Rml::Event& event) override {
        Rml::Element* target = with_id(event.GetTargetElement());
        if (!target) return;
        const Rml::String& type = event.GetType();
        std::string value = type == "change" ? change_value(event.GetTargetElement(), event)
                            : type == "submit" ? submit_value(event)
                                               : "null";
        std::vector<std::string> line = {prefix_ + type + " " + target->GetId() + " " + value};
        outbox::post(line);
    }

    // One listener serves every event of its document, and goes with the document.
    void OnAttach(Rml::Element*) override { ++attached_; }
    void OnDetach(Rml::Element*) override {
        if (--attached_ == 0) delete this;
    }

    void attach(Rml::ElementDocument* document) {
        for (const char* type : kEvents) document->AddEventListener(type, this);
    }

private:
    std::string prefix_;
    int attached_ = 0;
};

}  // namespace

void listen(Rml::ElementDocument* document, const std::string& mod, const std::string& doc) {
    (new Listener(mod, doc))->attach(document);
}

}  // namespace element_events
