#version 450

// One triangle over the whole target. The UV window can be moved and scaled, which is how
// blur scales down and drop-shadow offsets.

layout(push_constant) uniform Pass {
    vec2 uvOffset;
    vec2 uvScale;
} pass;

layout(location = 0) out vec2 uv;

void main() {
    vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = corner * pass.uvScale + pass.uvOffset;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
