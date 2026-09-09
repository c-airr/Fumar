#version 450

// The wireframe overlay: a flat, unlit colour.
//
// Lighting an outline would be wrong - it is not a surface, it is an
// annotation, and it has to stay legible against both a lit face and the
// background.

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vWorldPosition;

layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;

// The block must match mesh.vert exactly: one push constant range is shared by
// both stages of the pipeline, not two independent ones.
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColor;
    float highlight;
} object;

layout(location = 0) out vec4 outColor;

// Brighter than the interface accent, because a one-pixel line against a lit
// surface needs the extra contrast to read.
const vec3 kHoverColor = vec3(0.95, 0.62, 0.32);
const vec3 kSelectedColor = vec3(1.0, 0.78, 0.45);

void main() {
    // highlight carries which state this is: the renderer passes 1.0 for the
    // selected object and less for a merely hovered one.
    vec3 colour = object.highlight >= 0.99 ? kSelectedColor : kHoverColor;
    outColor = vec4(colour, 1.0);
}
