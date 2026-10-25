#version 450

layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Pass {
    layout(offset = 24) vec2 texMin;
    vec2 texMax;
    layout(offset = 48) vec4 weights;
} pass;

layout(location = 0) in vec2 taps[7];
layout(location = 0) out vec4 colour;

void main() {
    vec4 sum = vec4(0.0);
    for (int i = 0; i < 7; i++) {
        vec2 inside = step(pass.texMin, taps[i]) * step(taps[i], pass.texMax);
        sum += texture(source, taps[i]) * inside.x * inside.y * pass.weights[abs(i - 3)];
    }
    colour = sum;
}
