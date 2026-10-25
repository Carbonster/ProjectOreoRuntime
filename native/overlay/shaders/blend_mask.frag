#version 450

// mask-image: the layer, kept only where the mask has alpha.

layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 2, binding = 0) uniform sampler2D mask;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;

void main() {
    colour = texture(source, uv) * texture(mask, uv).a;
}
