#include "render/rml/rml_renderer.h"

#include <cstring>
#include <vector>

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/Vertex.h>

#include "log.h"
#include "render/rml/filters.h"
#include "render/rml/frame_arena.h"
#include "render/rml/gradients.h"
#include "render/rml/graveyard.h"
#include "render/rml/image_decoder.h"
#include "render/rml/pipelines.h"
#include "render/rml/recorder.h"
#include "render/rml/targets.h"
#include "render/rml/textures.h"

namespace rml_renderer {

namespace {

using recorder::Scratch;

// RmlUi promises the spans stay valid until ReleaseGeometry.
struct Geometry {
    Rml::Span<const Rml::Vertex> vertices;
    Rml::Span<const int> indices;
};

struct DrawPush {
    float transform[16];
    float translate[2];
};

// Vulkan's clip space runs 0..1 in depth where OpenGL's runs -1..1.
Rml::Matrix4f projection_for(VkExtent2D extent) {
    Rml::Matrix4f depth = Rml::Matrix4f::FromRows({1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f},
                                                  {0.0f, 0.0f, 0.5f, 0.5f}, {0.0f, 0.0f, 0.0f, 1.0f});
    return depth * Rml::Matrix4f::ProjectOrtho(0.0f, (float)extent.width, 0.0f,
                                               (float)extent.height, -10000.0f, 10000.0f);
}

class Renderer final : public Rml::RenderInterface {
public:
    bool ready = false;

    void begin(VkExtent2D extent) {
        projection_ = projection_for(extent);
        transform_ = projection_;
        scissor_ = Rml::Rectanglei::MakeInvalid();
        clip_ = false;
        stencilReference_ = 0;
        layers_ = 1;
        if (recorder::open_layer(0)) {
            recorder::clear_colour(scissor_);
            recorder::clear_stencil(0);
        }
    }

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override {
        return reinterpret_cast<Rml::CompiledGeometryHandle>(new Geometry{vertices, indices});
    }

    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override {
        if (!ready || !recorder::open_layer(top())) return;
        bind_program(texture ? Program::Texture : Program::Colour, clip_);
        if (texture) recorder::bind_set(pipelines::kTextureSet, as_texture(texture)->set);
        submit(geometry, translation);
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override {
        delete reinterpret_cast<Geometry*>(geometry);
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override {
        std::string bytes;
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> rgba;
        if (!ready || !Rml::GetFileInterface()->LoadFile(source, bytes) ||
            !image_decoder::decode(bytes, w, h, rgba)) {
            Rml::Log::Message(Rml::Log::LT_WARNING, "Cannot load image %s", source.c_str());
            return {};
        }
        dimensions = {(int)w, (int)h};
        return reinterpret_cast<Rml::TextureHandle>(textures::from_pixels(rgba.data(), w, h));
    }

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> pixels,
                                       Rml::Vector2i size) override {
        if (!ready || size.x <= 0 || size.y <= 0) return {};
        return reinterpret_cast<Rml::TextureHandle>(
            textures::from_pixels(pixels.data(), (uint32_t)size.x, (uint32_t)size.y));
    }

    void ReleaseTexture(Rml::TextureHandle texture) override {
        textures::release(as_texture(texture));
    }

    void EnableScissorRegion(bool enable) override {
        if (!enable) scissor_ = Rml::Rectanglei::MakeInvalid();
    }

    void SetScissorRegion(Rml::Rectanglei region) override { scissor_ = region; }

    void EnableClipMask(bool enable) override { clip_ = enable; }

    void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry,
                          Rml::Vector2f translation) override {
        if (!ready || !recorder::open_layer(top())) return;
        Program program = Program::ClipSet;
        uint32_t write = 1, test = 1;
        switch (operation) {
            case Rml::ClipMaskOperation::Set:
                recorder::clear_stencil(0);
                break;
            case Rml::ClipMaskOperation::SetInverse:
                recorder::clear_stencil(1);
                write = 0;
                break;
            case Rml::ClipMaskOperation::Intersect:
                program = Program::ClipIntersect;
                test = stencilReference_ + 1;
                break;
        }
        stencilReference_ = write;
        bind_program(program, false);
        submit(geometry, translation);
        stencilReference_ = test;
    }

    void SetTransform(const Rml::Matrix4f* transform) override {
        transform_ = transform ? projection_ * *transform : projection_;
    }

    Rml::LayerHandle PushLayer() override {
        if (layers_ < targets::kMaxLayers) {
            ++layers_;
        } else {
            Rml::Log::Message(Rml::Log::LT_WARNING, "Layers nested deeper than %d; drawing flat.",
                              targets::kMaxLayers);
        }
        if (ready && recorder::open_layer(top())) recorder::clear_colour(scissor_);
        return (Rml::LayerHandle)top();
    }

    void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination,
                         Rml::BlendMode blend,
                         Rml::Span<const Rml::CompiledFilterHandle> filters) override {
        if (!ready) return;
        recorder::resolve_layer((int)source, Scratch::Primary, scissor_);
        filters::run(filters, scissor_);
        if (!recorder::open_layer((int)destination)) return;
        Program program = blend == Rml::BlendMode::Replace ? Program::CompositeReplace
                                                           : Program::Composite;
        bind_program(program, clip_);
        draw_scratch(Scratch::Primary);
    }

    void PopLayer() override {
        if (layers_ > 1) --layers_;
    }

    Rml::TextureHandle SaveLayerAsTexture() override {
        if (!ready || !scissor_.Valid()) return {};
        recorder::close();
        VkExtent2D size = {(uint32_t)scissor_.Width(), (uint32_t)scissor_.Height()};
        Texture* texture = textures::blank(recorder::cmd(), size);
        if (!texture) return {};
        recorder::resolve_layer(top(), Scratch::Primary, scissor_);
        recorder::copy_to_origin(Scratch::Primary, scissor_, texture->image.image);
        return reinterpret_cast<Rml::TextureHandle>(texture);
    }

    Rml::CompiledFilterHandle SaveLayerAsMaskImage() override {
        if (!ready) return {};
        recorder::resolve_layer(top(), Scratch::Primary, scissor_);
        if (!recorder::open_mask()) return {};
        bind_program(Program::Copy, false);
        draw_scratch(Scratch::Primary);
        return filters::mask_image();
    }

    Rml::CompiledFilterHandle CompileFilter(const Rml::String& name,
                                            const Rml::Dictionary& parameters) override {
        return filters::compile(name, parameters);
    }

    void ReleaseFilter(Rml::CompiledFilterHandle filter) override { filters::release(filter); }

    Rml::CompiledShaderHandle CompileShader(const Rml::String& name,
                                            const Rml::Dictionary& parameters) override {
        return gradients::compile(name, parameters);
    }

    void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry,
                      Rml::Vector2f translation, Rml::TextureHandle) override {
        if (!ready || !recorder::open_layer(top()) || !gradients::bind(shader, clip_)) return;
        set_dynamic_state();
        submit(geometry, translation);
    }

    void ReleaseShader(Rml::CompiledShaderHandle shader) override { gradients::release(shader); }

    // Onto whatever target is open, which for the frame is the game's own image.
    void present() {
        recorder::bind(Program::Present);
        recorder::set_viewport(recorder::extent());
        recorder::set_scissor(Rml::Rectanglei::MakeInvalid());
        draw_scratch(Scratch::Primary);
    }

private:
    Rml::Matrix4f projection_ = Rml::Matrix4f::Identity();
    Rml::Matrix4f transform_ = Rml::Matrix4f::Identity();
    Rml::Rectanglei scissor_ = Rml::Rectanglei::MakeInvalid();
    bool clip_ = false;
    uint32_t stencilReference_ = 0;
    int layers_ = 1;

    int top() const { return layers_ - 1; }

    static Texture* as_texture(Rml::TextureHandle handle) {
        return reinterpret_cast<Texture*>(handle);
    }

    void set_dynamic_state() {
        recorder::set_viewport(recorder::extent());
        recorder::set_scissor(scissor_);
        recorder::set_stencil_reference(stencilReference_);
    }

    void bind_program(Program program, bool clipped) {
        recorder::bind(program, clipped);
        set_dynamic_state();
    }

    void submit(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation) {
        const Geometry& geometry = *reinterpret_cast<const Geometry*>(handle);
        VkDeviceSize vertexBytes = geometry.vertices.size() * sizeof(Rml::Vertex);
        VkDeviceSize indexBytes = geometry.indices.size() * sizeof(int);
        ArenaSpan span = frame_arena::allocate(vertexBytes + indexBytes, 4);
        if (!span.buffer || geometry.indices.empty()) return;
        memcpy(span.data, geometry.vertices.data(), (size_t)vertexBytes);
        memcpy((char*)span.data + vertexBytes, geometry.indices.data(), (size_t)indexBytes);

        DrawPush push{};
        memcpy(push.transform, transform_.data(), sizeof(push.transform));
        push.translate[0] = translation.x;
        push.translate[1] = translation.y;
        recorder::push(&push, sizeof(push));
        recorder::draw_indexed(span.buffer, span.offset, span.offset + vertexBytes,
                               (uint32_t)geometry.indices.size());
    }

    // The scratch image over the open target, as it is.
    void draw_scratch(Scratch scratch) {
        const DrawTarget* source = targets::post(recorder::scratch_index(scratch));
        if (!source) return;
        struct {
            float uvOffset[2] = {0.0f, 0.0f};
            float uvScale[2] = {1.0f, 1.0f};
            float factor = 1.0f;
        } push;
        recorder::bind_set(pipelines::kTextureSet, source->set);
        recorder::push(&push, sizeof(push));
        recorder::draw_fullscreen();
    }
};

Renderer g_renderer;

}  // namespace

Rml::RenderInterface* get() { return &g_renderer; }

bool create_device_objects() {
    g_renderer.ready = targets::create_passes() && pipelines::create() &&
                       textures::create(pipelines::texture_layout()) &&
                       frame_arena::create(pipelines::uniform_layout());
    if (!g_renderer.ready) LOG_ERROR("renderer: the UI will not be drawn.");
    return g_renderer.ready;
}

bool create_frame_objects(VkRenderPass framePass) {
    return g_renderer.ready && pipelines::create_present(framePass);
}

void destroy_frame_objects() {
    pipelines::destroy_present();
    targets::destroy();
    graveyard::empty();
}

void after_fence(uint32_t slot) {
    graveyard::begin_frame(slot);
    frame_arena::begin_frame(slot);
}

void frame_done(bool submitted) { graveyard::end_frame(submitted); }

void begin_frame(VkCommandBuffer cmd, VkExtent2D extent) {
    recorder::begin(cmd, extent);
    targets::resize(extent);
    if (g_renderer.ready) g_renderer.begin(extent);
}

bool end_frame() {
    if (!g_renderer.ready || !targets::layer(0)) {
        recorder::end();
        return false;
    }
    recorder::resolve_layer(0, Scratch::Primary, Rml::Rectanglei::MakeInvalid());
    recorder::end();
    return targets::post(recorder::scratch_index(Scratch::Primary)) != nullptr;
}

void present() { g_renderer.present(); }

}  // namespace rml_renderer
