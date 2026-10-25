#include "ui/polygon_element.h"

#include <charconv>
#include <string>
#include <vector>

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/PropertyIdSet.h>
#include <RmlUi/Core/RenderManager.h>

namespace polygon_element {

namespace {

constexpr size_t kMaxPoints = 64;

// "x,y x,y ...": numbers separated by commas or spaces, read the same in every locale.
std::vector<Rml::Vector2f> parse_points(const std::string& text) {
    std::vector<float> numbers;
    const char* p = text.data();
    const char* end = p + text.size();
    while (p < end && numbers.size() < kMaxPoints * 2) {
        while (p < end && (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n')) ++p;
        float value = 0.0f;
        auto result = std::from_chars(p, end, value);
        if (result.ec != std::errc()) break;
        numbers.push_back(value);
        p = result.ptr;
    }
    std::vector<Rml::Vector2f> points;
    for (size_t i = 0; i + 1 < numbers.size(); i += 2) points.push_back({numbers[i], numbers[i + 1]});
    return points;
}

class Polygon final : public Rml::Element {
public:
    using Rml::Element::Element;

protected:
    void OnRender() override {
        if (dirty_) build();
        geometry_.Render(GetAbsoluteOffset(Rml::BoxArea::Border));
    }

    void OnAttributeChange(const Rml::ElementAttributes& changed) override {
        Rml::Element::OnAttributeChange(changed);
        if (changed.count("points")) dirty_ = true;
    }

    void OnPropertyChange(const Rml::PropertyIdSet& changed) override {
        Rml::Element::OnPropertyChange(changed);
        if (changed.Contains(Rml::PropertyId::Color) || changed.Contains(Rml::PropertyId::Opacity))
            dirty_ = true;
    }

private:
    Rml::Geometry geometry_;
    bool dirty_ = true;

    // A fan from the first point: right for every convex shape, which is what this is for.
    void build() {
        dirty_ = false;
        std::vector<Rml::Vector2f> points = parse_points(GetAttribute<Rml::String>("points", ""));
        Rml::Mesh mesh;
        const Rml::ComputedValues& computed = GetComputedValues();
        Rml::ColourbPremultiplied colour = computed.color().ToPremultiplied(computed.opacity());
        for (const Rml::Vector2f& point : points) mesh.vertices.push_back({point, colour, {0, 0}});
        for (int i = 1; i + 1 < (int)points.size(); ++i) {
            mesh.indices.push_back(0);
            mesh.indices.push_back(i);
            mesh.indices.push_back(i + 1);
        }
        geometry_ = mesh ? GetRenderManager()->MakeGeometry(std::move(mesh)) : Rml::Geometry();
    }
};

Rml::ElementInstancerGeneric<Polygon>* g_instancer = nullptr;   // RmlUi keeps a pointer

}  // namespace

void register_with_rmlui() {
    if (!g_instancer) g_instancer = new Rml::ElementInstancerGeneric<Polygon>();
    Rml::Factory::RegisterElementInstancer("polygon", g_instancer);
}

}  // namespace polygon_element
