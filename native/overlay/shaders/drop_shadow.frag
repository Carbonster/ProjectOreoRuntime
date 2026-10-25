#version 450

// The layer's alpha, tinted. Adapted from RmlUi's Vulkan backend (MIT).

layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Pass {
    layout(offset = 16) vec2 texMin;
    vec2 texMax;
    vec4 tint;
} pass;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;

void main() {
    vec2 inside = step(pass.texMin, uv) * step(uv, pass.texMax);
    colour = texture(source, uv).a * inside.x * inside.y * pass.tint;
}
