#version 450

// Turns the HDR scene into something a monitor can show.
//
// The scene is rendered into a floating-point image where the sun is worth
// dozens and a shadowed corner a fraction of one - real ratios, not values
// pre-squashed into 0..1. A display cannot show that range, so the last step of
// the frame decides how to fit it: multiply by the exposure, run it through a
// curve that rolls the highlights off instead of clipping them, and write the
// result into the 8-bit sRGB image the interface displays.
//
// Skip this and adding a real sun makes everything above mid-grey turn into one
// flat white shape.

#include "frame.glsl"

// Set 1 is the per-material set elsewhere in the engine; here it carries the
// HDR scene image. Reusing the layout rather than inventing a second one that
// happens to be identical keeps the pipeline layouts interchangeable.
layout(set = 1, binding = 0) uniform sampler2D hdrScene;

layout(location = 0) in vec2 vUV;

layout(location = 0) out vec4 outColor;

/// ACES filmic curve, Krzysztof Narkowicz's fit.
///
/// The full ACES transform is a chain of matrices and a spline; this is a
/// rational function fitted to it that is within a rounding error visually and
/// costs a handful of instructions. What matters is the SHAPE: bright values
/// compress smoothly towards white instead of hitting a wall, so a highlight
/// keeps its colour as it gets brighter rather than turning into a white blob.
vec3 tonemapACES(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 colour = texture(hdrScene, vUV).rgb * frame.exposure;
    colour = tonemapACES(colour);

    // No pow(1/2.2) here: the target image is an sRGB format, so the hardware
    // applies the transfer function when writing. Doing it here as well would
    // wash the picture out - a classic double-gamma.
    outColor = vec4(colour, 1.0);
}
