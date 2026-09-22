#version 450
// Unlit textured: the SRGB-sampled base color decodes to linear, so this stage
// emits LINEAR — and exactly one encode must follow it (INV-4.6 / ADR-021):
//   * sRGB internal target  → the attachment's hardware write encodes, and the
//     _SRGB→_SRGB blit to the swapchain decodes+re-encodes (identity);
//   * UNORM internal target → nothing encodes for us, so we encode here.
// pc.tint.a is that gate (1 = encode here, 0 = the attachment does it); getting
// it wrong lands the view a whole gamma out.

layout(location = 0) in vec2 inUV;

layout(set = 0, binding = 0) uniform sampler2D baseColor;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 tint;
} pc;

layout(location = 0) out vec4 outColor;

// Inverse sRGB EOTF (accurate piecewise) — standard sRGB only, no vendor curve.
vec3 linearToSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, vec3(lessThan(c, vec3(0.0031308))));
}

void main()
{
    vec3 color = texture(baseColor, inUV).rgb * pc.tint.rgb;
    if (pc.tint.a > 0.5) color = linearToSrgb(color);
    outColor = vec4(color, 1.0);
}
