#version 450
// Unlit textured: SRGB-sampled base color → sRGB attachment re-encodes
// (INV-4.6 round-trip; no manual gamma).

layout(location = 0) in vec2 inUV;

layout(set = 0, binding = 0) uniform sampler2D baseColor;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 tint;
} pc;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(texture(baseColor, inUV).rgb * pc.tint.rgb, 1.0);
}
