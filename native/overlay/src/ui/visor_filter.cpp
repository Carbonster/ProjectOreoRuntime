#include "ui/visor_filter.h"

#include <RmlUi/Core/CompiledFilterShader.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Filter.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/RenderManager.h>

namespace visor_filter {

namespace {

// What the game's own HUD shader uses.
constexpr const char* kSqueeze = "0.1";
constexpr const char* kShift = "0.03";

class Visor final : public Rml::Filter {
public:
    Visor(float squeeze, float shift) : squeeze_(squeeze), shift_(shift) {}

    Rml::CompiledFilter CompileFilter(Rml::Element* element) const override {
        return element->GetRenderManager()->CompileFilter(
            "visor", Rml::Dictionary{{"squeeze", Rml::Variant(squeeze_)},
                                     {"shift", Rml::Variant(shift_)}});
    }

    // The bend is measured across the whole screen, and moves the element anywhere on it.
    void ExtendInkOverflow(Rml::Element* element, Rml::Rectanglef& region) const override {
        if (Rml::Context* context = element->GetContext())
            region = Rml::Rectanglef::FromSize(Rml::Vector2f(context->GetDimensions()));
    }

private:
    float squeeze_, shift_;
};

class VisorInstancer final : public Rml::FilterInstancer {
public:
    VisorInstancer() {
        squeeze_ = RegisterProperty("squeeze", kSqueeze).AddParser("number").GetId();
        shift_ = RegisterProperty("shift", kShift).AddParser("number").GetId();
        RegisterShorthand("filter", "squeeze, shift", Rml::ShorthandType::FallThrough);
    }

    Rml::SharedPtr<Rml::Filter> InstanceFilter(const Rml::String&,
                                               const Rml::PropertyDictionary& properties) override {
        const Rml::Property* squeeze = properties.GetProperty(squeeze_);
        const Rml::Property* shift = properties.GetProperty(shift_);
        if (!squeeze || !shift) return nullptr;
        return Rml::MakeShared<Visor>(squeeze->Get<float>(), shift->Get<float>());
    }

private:
    Rml::PropertyId squeeze_, shift_;
};

VisorInstancer* g_instancer = nullptr;   // RmlUi keeps a pointer; it lives as long as we do

}  // namespace

void register_with_rmlui() {
    if (!g_instancer) g_instancer = new VisorInstancer();
    Rml::Factory::RegisterFilterInstancer("visor", g_instancer);
}

}  // namespace visor_filter
