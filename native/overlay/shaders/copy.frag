#version 450

// A layer copied as it is, or faded: opacity is this with a factor below one.

layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Pass {
    layout(offset = 16) float factor;
} pass;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;

void main() {
    colour = texture(source, uv) * pass.factor;
}
