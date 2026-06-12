#version 450
// Unlit textured tile pass — photogrammetry has baked lighting (PRD §5).
// Positions are tile-local floats; the push-constant MVP is the per-tile
// anchor-folded float matrix built in double on the CPU (RTC scheme, PRD §6.1).

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 tint; // rgb tint × a=1 for textureless primitives (white tex bound)
} pc;

layout(location = 0) out vec2 outUV;

void main()
{
    outUV = inUV;
    gl_Position = pc.mvp * vec4(inPos, 1.0);
}
