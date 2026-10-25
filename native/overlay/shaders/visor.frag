#version 450

// The UI as if projected onto the helmet visor, the way the game draws its own HUD.
// No distortion along x. Along y the middle of the screen is squeezed by 1 / (1 + squeeze),
// the left and right edges not at all, eased by a sine: the top and bottom of the HUD bow
// towards the centre. Everything moves down by shift / 2 of the screen height.

layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Pass {
    layout(offset = 16) float squeeze;
    float shift;
} pass;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;

void main() {
    vec2 p = uv * 2.0 - 1.0;
    float k = mix(1.0, 1.0 + pass.squeeze, sin(1.5707963 * (p.x + 1.0)));
    p.y = p.y * k - pass.shift;
    vec2 s = (p + 1.0) * 0.5;
    colour = (s.y < 0.0 || s.y > 1.0) ? vec4(0.0) : texture(source, s);
}
