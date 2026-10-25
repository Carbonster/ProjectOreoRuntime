#version 450

// RmlUi's own geometry: boxes, borders, text quads.

layout(push_constant) uniform Draw {
    mat4 transform;
    vec2 translate;
} draw;

layout(location = 0) in vec2 position;
layout(location = 1) in vec4 colour;
layout(location = 2) in vec2 texCoord;

layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 tint;

void main() {
    uv = texCoord;
    tint = colour;
    gl_Position = draw.transform * vec4(position + draw.translate, 0.0, 1.0);
}
