#version 450

layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 tint;
layout(location = 0) out vec4 colour;

void main() {
    colour = tint;
}
