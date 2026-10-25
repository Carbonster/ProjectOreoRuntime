#include "render/rml/gradients.h"

#include <algorithm>
#include <cmath>

#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Log.h>

#include "render/rml/frame_arena.h"
#include "render/rml/recorder.h"

namespace gradients {

namespace {

constexpr int kMaxStops = 16;

// Matches gradient.frag, std140.
struct Uniforms {
    int32_t func;
    int32_t count;
    float p[2];
    float v[2];
    float padding[2];
    float colours[kMaxStops][4];
    float positions[kMaxStops];
};
static_assert(sizeof(Uniforms) <= frame_arena::kUniformRange, "fits the uniform range");

enum Func { Linear, Radial, Conic, RepeatingLinear, RepeatingRadial, RepeatingConic };

void read_stops(const Rml::Dictionary& parameters, Uniforms& out) {
    auto it = parameters.find("color_stop_list");
    if (it == parameters.end() || it->second.GetType() != Rml::Variant::COLORSTOPLIST) return;
    const Rml::ColorStopList& stops = it->second.GetReference<Rml::ColorStopList>();
    out.count = (std::min)((int)stops.size(), kMaxStops);
    for (int i = 0; i < out.count; ++i) {
        out.positions[i] = stops[i].position.number;
        Rml::ColourbPremultiplied c = stops[i].color;
        out.colours[i][0] = c.red / 255.0f;
        out.colours[i][1] = c.green / 255.0f;
        out.colours[i][2] = c.blue / 255.0f;
        out.colours[i][3] = c.alpha / 255.0f;
    }
}

void set(float (&to)[2], Rml::Vector2f from) {
    to[0] = from.x;
    to[1] = from.y;
}

}  // namespace

Rml::CompiledShaderHandle compile(const Rml::String& name, const Rml::Dictionary& parameters) {
    Uniforms gradient{};
    int repeat = Rml::Get(parameters, "repeating", false) ? RepeatingLinear : 0;
    if (name == "linear-gradient") {
        gradient.func = Linear + repeat;
        Rml::Vector2f p0 = Rml::Get(parameters, "p0", Rml::Vector2f(0.0f));
        set(gradient.p, p0);
        set(gradient.v, Rml::Get(parameters, "p1", Rml::Vector2f(0.0f)) - p0);
    } else if (name == "radial-gradient") {
        gradient.func = Radial + repeat;
        set(gradient.p, Rml::Get(parameters, "center", Rml::Vector2f(0.0f)));
        set(gradient.v, Rml::Vector2f(1.0f) / Rml::Get(parameters, "radius", Rml::Vector2f(1.0f)));
    } else if (name == "conic-gradient") {
        gradient.func = Conic + repeat;
        set(gradient.p, Rml::Get(parameters, "center", Rml::Vector2f(0.0f)));
        float angle = Rml::Get(parameters, "angle", 0.0f);
        set(gradient.v, {std::cos(angle), std::sin(angle)});
    } else {
        Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported shader '%s'.", name.c_str());
        return {};
    }
    read_stops(parameters, gradient);
    if (gradient.count == 0) return {};
    return reinterpret_cast<Rml::CompiledShaderHandle>(new Uniforms(gradient));
}

void release(Rml::CompiledShaderHandle shader) { delete reinterpret_cast<Uniforms*>(shader); }

bool bind(Rml::CompiledShaderHandle shader, bool clipped) {
    ArenaSpan block = frame_arena::push_uniform(reinterpret_cast<const Uniforms*>(shader),
                                                sizeof(Uniforms));
    if (!block.buffer) return false;
    recorder::bind(Program::Gradient, clipped);
    recorder::bind_uniform(block.uniforms, (uint32_t)block.offset);
    return true;
}

}  // namespace gradients
