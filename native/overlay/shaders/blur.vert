#version 450

// One direction of a 7-tap Gaussian. Adapted from RmlUi's Vulkan backend (MIT).

layout(push_constant) uniform Pass {
    vec2 uvOffset;
    vec2 uvScale;
    vec2 texelOffset;
} pass;

layout(location = 0) out vec2 taps[7];

void main() {
    vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vec2 uv = corner * pass.uvScale + pass.uvOffset;
    for (int i = 0; i < 7; i++)
        taps[i] = uv - float(i - 3) * pass.texelOffset;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
