// RmlUi with nowhere to draw: layout, data bindings and events run as in the game.
#pragma once

#include <RmlUi/Core/RenderInterface.h>

class NoGpu final : public Rml::RenderInterface {
public:
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>,
                                                Rml::Span<const int>) override {
        return 1;
    }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    // Every image asked for, by the path RmlUi resolved it to.
    Rml::Vector<Rml::String> images;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& size, const Rml::String& source) override {
        images.push_back(source);
        size = {1, 1};
        return 1;
    }
    // Every texture made from pixels: glyph atlases, rendered SVG.
    Rml::Vector<Rml::Vector2i> generated;

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i size) override {
        generated.push_back(size);
        return 1;
    }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
    // Gradients, filters and shadows compile to something, as on the GPU.
    Rml::CompiledShaderHandle CompileShader(const Rml::String&, const Rml::Dictionary&) override {
        return 1;
    }
    void RenderShader(Rml::CompiledShaderHandle, Rml::CompiledGeometryHandle, Rml::Vector2f,
                      Rml::TextureHandle) override {}
    void ReleaseShader(Rml::CompiledShaderHandle) override {}
    Rml::CompiledFilterHandle CompileFilter(const Rml::String&, const Rml::Dictionary&) override {
        return 1;
    }
    void ReleaseFilter(Rml::CompiledFilterHandle) override {}
    Rml::LayerHandle PushLayer() override { return 1; }
    void CompositeLayers(Rml::LayerHandle, Rml::LayerHandle, Rml::BlendMode,
                         Rml::Span<const Rml::CompiledFilterHandle>) override {}
    void PopLayer() override {}
    Rml::TextureHandle SaveLayerAsTexture() override { return 1; }
};
