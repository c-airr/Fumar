#version 450

// The wireframe overlay: a flat, unlit colour.
//
// Lighting an outline would be wrong - it is not a surface, it is an
// annotation, and it has to stay legible against both a lit face and the sky.

#include "object.glsl"

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vWorldPosition;

layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;

layout(location = 0) out vec4 outColor;

// Values above 1, because this is written into the HDR target and the tone
// mapper compresses everything on the way out: feed it 1.0 and the line comes
// back at about 0.8, a washed-out grey against a bright sky. Around 3 lands
// near white with the highlight roll-off still intact.
const vec3 kHoverColor = vec3(2.9, 1.9, 1.0);
const vec3 kSelectedColor = vec3(3.6, 2.8, 1.6);

void main() {
    // highlight carries which state this is: the renderer passes 1.0 for the
    // selected object and less for a merely hovered one.
    const vec3 colour = object.highlight >= 0.99 ? kSelectedColor : kHoverColor;
    outColor = vec4(colour, 1.0);
}
