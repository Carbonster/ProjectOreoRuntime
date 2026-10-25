#version 450

// CSS gradients. Adapted from RmlUi's Vulkan backend (MIT).

#define LINEAR 0
#define RADIAL 1
#define CONIC 2
#define REPEATING_LINEAR 3
#define REPEATING_RADIAL 4
#define REPEATING_CONIC 5
#define PI 3.14159265

layout(set = 1, binding = 0) uniform Gradient {
    int func;
    int count;
    vec2 p;   // linear: start, radial and conic: centre
    vec2 v;   // linear: start to end, radial: inverse radius, conic: unit vector of the angle
    vec4 colours[16];
    vec4 positions[4];   // 16 stop positions, four to a vector
} gradient;

layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 tint;
layout(location = 0) out vec4 colour;

float stop(int i) {
    return gradient.positions[i >> 2][i & 3];
}

vec4 colour_at(float t) {
    vec4 c = gradient.colours[0];
    for (int i = 1; i < gradient.count; i++)
        c = mix(c, gradient.colours[i], smoothstep(stop(i - 1), stop(i), t));
    return c;
}

void main() {
    int func = gradient.func;
    vec2 d = uv - gradient.p;
    float t = 0.0;
    if (func == LINEAR || func == REPEATING_LINEAR) {
        t = dot(gradient.v, d) / dot(gradient.v, gradient.v);
    } else if (func == RADIAL || func == REPEATING_RADIAL) {
        t = length(gradient.v * d);
    } else {
        mat2 r = mat2(gradient.v.x, -gradient.v.y, gradient.v.y, gradient.v.x);
        vec2 q = r * d;
        t = 0.5 + atan(-q.x, q.y) / (2.0 * PI);
    }
    if (func >= REPEATING_LINEAR) {
        float t0 = stop(0);
        float t1 = stop(gradient.count - 1);
        t = t0 + mod(t - t0, t1 - t0);
    }
    colour = tint * colour_at(t);
}
