// The filter passes follow RmlUi's GL3 renderer (MIT), moved to top-left coordinates.
#include "render/rml/filters.h"

#include <cmath>

#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/Math.h>

#include "render/rml/recorder.h"
#include "render/rml/targets.h"

namespace filters {

namespace {

using recorder::Scratch;

enum class Kind { Opacity, Blur, DropShadow, ColourMatrix, MaskImage, Visor };

struct Filter {
    Kind kind;
    float value = 1.0f;   // opacity
    float sigma = 0.0f;   // blur, drop-shadow
    Rml::Vector2f offset;
    Rml::Colourf colour;   // premultiplied
    Rml::Matrix4f matrix = Rml::Matrix4f::Identity();
    float visor[2] = {0.0f, 0.0f};   // squeeze, shift
};

// The colour matrices of the Filter Effects spec, by name.
using MatrixFor = Rml::Matrix4f (*)(float value);

Rml::Matrix4f brightness(float v) { return Rml::Matrix4f::Diag(v, v, v, 1.0f); }

Rml::Matrix4f contrast(float v) {
    float grey = 0.5f - 0.5f * v;
    Rml::Matrix4f m = Rml::Matrix4f::Diag(v, v, v, 1.0f);
    m.SetColumn(3, Rml::Vector4f(grey, grey, grey, 1.0f));
    return m;
}

Rml::Matrix4f invert(float v) {
    v = Rml::Math::Clamp(v, 0.0f, 1.0f);
    float k = 1.0f - 2.0f * v;
    Rml::Matrix4f m = Rml::Matrix4f::Diag(k, k, k, 1.0f);
    m.SetColumn(3, Rml::Vector4f(v, v, v, 1.0f));
    return m;
}

Rml::Matrix4f grayscale(float v) {
    float r = 1.0f - v;
    Rml::Vector3f g = v * Rml::Vector3f(0.2126f, 0.7152f, 0.0722f);
    return Rml::Matrix4f::FromRows({g.x + r, g.y, g.z, 0.0f}, {g.x, g.y + r, g.z, 0.0f},
                                   {g.x, g.y, g.z + r, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f});
}

Rml::Matrix4f sepia(float v) {
    float r = 1.0f - v;
    Rml::Vector3f a = v * Rml::Vector3f(0.393f, 0.769f, 0.189f);
    Rml::Vector3f b = v * Rml::Vector3f(0.349f, 0.686f, 0.168f);
    Rml::Vector3f c = v * Rml::Vector3f(0.272f, 0.534f, 0.131f);
    return Rml::Matrix4f::FromRows({a.x + r, a.y, a.z, 0.0f}, {b.x, b.y + r, b.z, 0.0f},
                                   {c.x, c.y, c.z + r, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f});
}

Rml::Matrix4f hue_rotate(float v) {
    float s = std::sin(v), c = std::cos(v);
    return Rml::Matrix4f::FromRows(
        {0.213f + 0.787f * c - 0.213f * s, 0.715f - 0.715f * c - 0.715f * s, 0.072f - 0.072f * c + 0.928f * s, 0.0f},
        {0.213f - 0.213f * c + 0.143f * s, 0.715f + 0.285f * c + 0.140f * s, 0.072f - 0.072f * c - 0.283f * s, 0.0f},
        {0.213f - 0.213f * c - 0.787f * s, 0.715f - 0.715f * c + 0.715f * s, 0.072f + 0.928f * c + 0.072f * s, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f});
}

Rml::Matrix4f saturate(float v) {
    return Rml::Matrix4f::FromRows(
        {0.213f + 0.787f * v, 0.715f - 0.715f * v, 0.072f - 0.072f * v, 0.0f},
        {0.213f - 0.213f * v, 0.715f + 0.285f * v, 0.072f - 0.072f * v, 0.0f},
        {0.213f - 0.213f * v, 0.715f - 0.715f * v, 0.072f + 0.928f * v, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f});
}

struct NamedMatrix {
    const char* name;
    MatrixFor matrix;
};
const NamedMatrix kMatrices[] = {
    {"brightness", brightness}, {"contrast", contrast},     {"invert", invert},
    {"grayscale", grayscale},   {"sepia", sepia},           {"hue-rotate", hue_rotate},
    {"saturate", saturate},
};

Rml::Colourf to_float(Rml::ColourbPremultiplied c) {
    return Rml::Colourf(c.red / 255.0f, c.green / 255.0f, c.blue / 255.0f, c.alpha / 255.0f);
}

// What every fullscreen pass pushes first: where in the source the triangle's UVs land.
struct Uv {
    Rml::Vector2f offset = {0.0f, 0.0f};
    Rml::Vector2f scale = {1.0f, 1.0f};
};
constexpr uint32_t kParamsOffset = sizeof(Uv);

void pass(Program program, Scratch from, Scratch to, Rml::Rectanglei scissor, const void* params,
          uint32_t bytes, Uv uv = {}, VkExtent2D viewport = {0, 0}) {
    const DrawTarget* source = targets::post(recorder::scratch_index(from));
    if (!source || !recorder::open_scratch(to)) return;
    recorder::bind(program);
    recorder::bind_set(0, source->set);
    recorder::set_viewport(viewport.width ? viewport : recorder::extent());
    recorder::set_scissor(scissor);
    recorder::push(&uv, sizeof(uv));
    if (bytes) recorder::push(params, bytes, kParamsOffset);
    recorder::draw_fullscreen();
}

void copy(Scratch from, Scratch to, Rml::Rectanglei scissor, float factor = 1.0f, Uv uv = {},
          VkExtent2D viewport = {0, 0}) {
    pass(Program::Copy, from, to, scissor, &factor, sizeof(factor), uv, viewport);
}

// Texture coordinates clamped to texel centres inside `region`, so bilinear lookups never
// bleed in from outside it.
void texel_limits(Rml::Rectanglei region, float (&min)[2], float (&max)[2]) {
    VkExtent2D size = recorder::extent();
    min[0] = (region.Left() + 0.5f) / size.width;
    min[1] = (region.Top() + 0.5f) / size.height;
    max[0] = (region.Right() - 0.5f) / size.width;
    max[1] = (region.Bottom() - 0.5f) / size.height;
}

void sigma_to_passes(float desired, int& level, float& sigma) {
    constexpr int kMaxPasses = 10;
    constexpr float kMaxSinglePass = 3.0f;
    level = Rml::Math::Clamp(Rml::Math::Log2(int(desired * (2.0f / kMaxSinglePass))), 0, kMaxPasses);
    sigma = Rml::Math::Clamp(desired / float(1 << level), 0.0f, kMaxSinglePass);
}

struct BlurParams {
    float texelOffset[2];
    float texMin[2];
    float texMax[2];
    float padding[2];
    float weights[4];
};

void blur_weights(float sigma, float (&weights)[4]) {
    float total = 0.0f;
    for (int i = 0; i < 4; ++i) {
        weights[i] = std::fabs(sigma) < 0.1f
                         ? float(i == 0)
                         : std::exp(-float(i * i) / (2.0f * sigma * sigma)) /
                               (std::sqrt(2.0f * Rml::Math::RMLUI_PI) * sigma);
        total += (i == 0 ? 1.0f : 2.0f) * weights[i];
    }
    for (float& w : weights) w /= total;
}

// Scales down by halves until one 7-tap pass is enough, blurs both ways, scales back up.
void blur(float desired, Scratch target, Scratch temp, Rml::Rectanglei window) {
    int level = 0;
    float sigma = 0.0f;
    sigma_to_passes(desired, level, sigma);
    VkExtent2D full = recorder::extent();
    VkExtent2D half = {full.width / 2, full.height / 2};

    Uv shrink;
    shrink.scale = {full.width % 2 ? 1.0f - 1.0f / full.width : 1.0f,
                    full.height % 2 ? 1.0f - 1.0f / full.height : 1.0f};
    Rml::Rectanglei scissor = window;
    for (int i = 0; i < level; ++i) {
        scissor.p0 = (scissor.p0 + Rml::Vector2i(1)) / 2;
        scissor.p1 = Rml::Math::Max(scissor.p1 / 2, scissor.p0);
        bool fromTarget = i % 2 == 0;
        copy(fromTarget ? target : temp, fromTarget ? temp : target, scissor, 1.0f, shrink, half);
    }
    if (level % 2 == 0) copy(target, temp, scissor);

    BlurParams params{};
    texel_limits(scissor, params.texMin, params.texMax);
    blur_weights(sigma, params.weights);
    params.texelOffset[1] = 1.0f / full.height;
    pass(Program::Blur, temp, target, scissor, &params, sizeof(params));

    if (recorder::open_scratch(temp)) recorder::clear_colour(scissor.Extend(1));
    params.texelOffset[0] = 1.0f / full.width;
    params.texelOffset[1] = 0.0f;
    pass(Program::Blur, target, temp, scissor, &params, sizeof(params));

    // The exact power-of-two scale is steadier when the blurred region moves; the first blit
    // fills the edges it can leave.
    recorder::blit(temp, scissor, target, window);
    Rml::Rectanglei exact = Rml::Rectanglei::FromCorners(scissor.p0 * (1 << level),
                                                         scissor.p1 * (1 << level));
    if (exact != window) recorder::blit(temp, scissor, target, exact);
}

void drop_shadow(const Filter& filter, Rml::Rectanglei region) {
    struct {
        float texMin[2];
        float texMax[2];
        Rml::Colourf colour;
    } params{};
    texel_limits(region, params.texMin, params.texMax);
    params.colour = filter.colour;
    VkExtent2D size = recorder::extent();
    Uv uv;
    uv.offset = {-filter.offset.x / size.width, -filter.offset.y / size.height};
    pass(Program::DropShadow, Scratch::Primary, Scratch::Secondary, region, &params,
         sizeof(params), uv);
    if (filter.sigma >= 0.5f) blur(filter.sigma, Scratch::Secondary, Scratch::Tertiary, region);
    float one = 1.0f;
    pass(Program::CopyBlend, Scratch::Primary, Scratch::Secondary, region, &one, sizeof(one));
    recorder::swap_primary();
}

void run_one(const Filter& filter, Rml::Rectanglei region) {
    switch (filter.kind) {
        case Kind::Opacity:
            copy(Scratch::Primary, Scratch::Secondary, region, filter.value);
            recorder::swap_primary();
            break;
        case Kind::Blur:
            blur(filter.sigma, Scratch::Primary, Scratch::Secondary, region);
            break;
        case Kind::DropShadow:
            drop_shadow(filter, region);
            break;
        case Kind::ColourMatrix:
            pass(Program::ColourMatrix, Scratch::Primary, Scratch::Secondary, region,
                 filter.matrix.data(), sizeof(float) * 16);
            recorder::swap_primary();
            break;
        case Kind::MaskImage: {
            const DrawTarget* mask = targets::post(targets::kMask);
            if (!mask) break;
            recorder::bind_set(pipelines::kMaskSet, mask->set);   // survives the pipeline bind
            pass(Program::BlendMask, Scratch::Primary, Scratch::Secondary, region, nullptr, 0);
            recorder::swap_primary();
            break;
        }
        case Kind::Visor:
            pass(Program::Visor, Scratch::Primary, Scratch::Secondary, region, filter.visor,
                 sizeof(filter.visor));
            recorder::swap_primary();
            break;
    }
}

Rml::CompiledFilterHandle keep(const Filter& filter) {
    return reinterpret_cast<Rml::CompiledFilterHandle>(new Filter(filter));
}

}  // namespace

Rml::CompiledFilterHandle compile(const Rml::String& name, const Rml::Dictionary& parameters) {
    float value = Rml::Get(parameters, "value", 1.0f);
    for (const NamedMatrix& entry : kMatrices) {
        if (name != entry.name) continue;
        Filter filter{Kind::ColourMatrix};
        filter.matrix = entry.matrix(value);
        return keep(filter);
    }
    if (name == "opacity") {
        Filter filter{Kind::Opacity};
        filter.value = value;
        return keep(filter);
    }
    if (name == "blur") {
        Filter filter{Kind::Blur};
        filter.sigma = Rml::Get(parameters, "sigma", 1.0f);
        return keep(filter);
    }
    if (name == "drop-shadow") {
        Filter filter{Kind::DropShadow};
        filter.sigma = Rml::Get(parameters, "sigma", 0.0f);
        filter.colour = to_float(Rml::Get(parameters, "color", Rml::Colourb()).ToPremultiplied());
        filter.offset = Rml::Get(parameters, "offset", Rml::Vector2f(0.0f));
        return keep(filter);
    }
    if (name == "visor") {
        Filter filter{Kind::Visor};
        filter.visor[0] = Rml::Get(parameters, "squeeze", 0.0f);
        filter.visor[1] = Rml::Get(parameters, "shift", 0.0f);
        return keep(filter);
    }
    Rml::Log::Message(Rml::Log::LT_WARNING, "Unsupported filter '%s'.", name.c_str());
    return {};
}

Rml::CompiledFilterHandle mask_image() { return keep(Filter{Kind::MaskImage}); }

void release(Rml::CompiledFilterHandle filter) { delete reinterpret_cast<Filter*>(filter); }

void run(Rml::Span<const Rml::CompiledFilterHandle> filters, Rml::Rectanglei region) {
    for (Rml::CompiledFilterHandle handle : filters)
        run_one(*reinterpret_cast<const Filter*>(handle), region);
}

}  // namespace filters
