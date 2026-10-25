#version 450

// brightness, contrast, grayscale, sepia, hue-rotate, saturate, invert.

layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Pass {
    layout(offset = 16) mat4 matrix;
} pass;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;

void main() {
    vec4 c = texture(source, uv);
    colour = vec4((pass.matrix * c).rgb, c.a);
}
