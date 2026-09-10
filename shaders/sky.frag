#version 450

// Draws the sky into every pixel of the frame, before any geometry.
//
// Cheaper than it looks: there is no skybox mesh, no cube map and no vertex
// buffer. The fullscreen triangle gives each pixel a clip-space position, the
// inverse view-projection turns that into a direction, and the sky is a
// function of direction.

#include "sky.glsl"

layout(location = 0) in vec2 vUV;

layout(location = 0) out vec4 outColor;

void main() {
    // The same NDC the rasteriser produced for this pixel: fullscreen.vert
    // writes gl_Position = vec4(vUV * 2 - 1, ...), so inverting that is exact
    // rather than approximate.
    const vec3 direction = worldRayDirection(vUV * 2.0 - 1.0);
    outColor = vec4(skyWithSun(direction), 1.0);
}
