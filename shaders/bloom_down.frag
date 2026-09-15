#version 450

// One step down the bloom chain: read a brighter, larger image, write a
// blurrier, half-sized one.
//
// Bloom exists because a camera - or an eye - is not a perfect instrument. Light
// far brighter than the rest of the frame scatters inside the lens and across
// the sensor, so a bright thing does not stop at its own silhouette: it spills
// past it. That spill is the only cue a display has for "this is brighter than
// the screen can show", because the screen runs out of white long before the
// scene runs out of light. Without it, the sun and a white wall are the same
// pixel value and the image reads as flat paint rather than as light.
//
// The blur is not done with one wide kernel. A blur that reaches a tenth of the
// screen would need hundreds of taps per pixel; instead the image is halved
// repeatedly, blurred a little at each size, and added back up on the way out.
// The same reach costs a handful of taps because the later levels are tiny.

layout(set = 0, binding = 0) uniform sampler2D source;

layout(push_constant) uniform PushConstants {
    /// One texel of the SOURCE, in UV. The filter below is defined in source
    /// texels, not destination ones.
    vec2 texelSize;

    /// Where "bright" starts. Everything below this contributes nothing.
    float threshold;

    /// Width of the soft shoulder below the threshold. A hard cutoff makes the
    /// glow pop in and out as a surface drifts past the threshold - visible as
    /// a crawling edge on anything that moves.
    float knee;

    /// 1 on the first step only, which is the one reading the raw scene.
    int prefilter;
} push;

layout(location = 0) in vec2 vUV;

layout(location = 0) out vec4 outColor;

float brightness(vec3 c) {
    return max(c.r, max(c.g, c.b));
}

/// Keeps the part of a colour that counts as "bright", with a soft shoulder.
vec3 prefilter(vec3 c) {
    const float b = brightness(c);

    // Quadratic ramp across the knee, so contribution grows from zero rather
    // than switching on.
    float soft = clamp(b - push.threshold + push.knee, 0.0, 2.0 * push.knee);
    soft = soft * soft / (4.0 * push.knee + 0.0001);

    return c * max(soft, b - push.threshold) / max(b, 0.0001);
}

/// Weight that pulls a single very bright sample back towards its neighbours.
///
/// One pixel of a specular highlight can be worth thousands while its
/// neighbours are worth one. Averaged plainly, that pixel decides the whole
/// block, and as the camera moves it flickers on and off - a firefly. Weighting
/// each group by 1/(1+brightness) before averaging is Brian Karis's fix: the
/// bright sample still dominates, but it can no longer dominate absolutely.
float karisWeight(vec3 c) {
    return 1.0 / (1.0 + brightness(c));
}

void main() {
    const vec2 t = push.texelSize;

    // The thirteen-tap pattern from Jimenez's SIGGRAPH 2014 talk on Call of
    // Duty's bloom. Four inner taps at the half-texel diagonals, eight outer
    // ones on a full-texel grid, and the centre. It is not one box filter but
    // five overlapping 2x2 boxes, which is why a chain built from it stays
    // smooth instead of developing the blocky pulsing a naive halving gives.
    const vec3 a = texture(source, vUV + vec2(-2.0, 2.0) * t).rgb;
    const vec3 b = texture(source, vUV + vec2(0.0, 2.0) * t).rgb;
    const vec3 c = texture(source, vUV + vec2(2.0, 2.0) * t).rgb;

    const vec3 d = texture(source, vUV + vec2(-2.0, 0.0) * t).rgb;
    const vec3 e = texture(source, vUV).rgb;
    const vec3 f = texture(source, vUV + vec2(2.0, 0.0) * t).rgb;

    const vec3 g = texture(source, vUV + vec2(-2.0, -2.0) * t).rgb;
    const vec3 h = texture(source, vUV + vec2(0.0, -2.0) * t).rgb;
    const vec3 i = texture(source, vUV + vec2(2.0, -2.0) * t).rgb;

    const vec3 j = texture(source, vUV + vec2(-1.0, 1.0) * t).rgb;
    const vec3 k = texture(source, vUV + vec2(1.0, 1.0) * t).rgb;
    const vec3 l = texture(source, vUV + vec2(-1.0, -1.0) * t).rgb;
    const vec3 m = texture(source, vUV + vec2(1.0, -1.0) * t).rgb;

    // Five 2x2 groups: the inner one counts for half, the four outer ones for
    // an eighth each.
    vec3 g0 = (j + k + l + m) * 0.25;
    vec3 g1 = (a + b + d + e) * 0.25;
    vec3 g2 = (b + c + e + f) * 0.25;
    vec3 g3 = (d + e + g + h) * 0.25;
    vec3 g4 = (e + f + h + i) * 0.25;

    vec3 result;
    if (push.prefilter != 0) {
        // Firefly suppression belongs on this step alone: it is the only one
        // reading the raw scene, where a single pixel can be worth thousands.
        // Every later step reads an image that has already been averaged.
        const float w0 = karisWeight(g0) * 0.5;
        const float w1 = karisWeight(g1) * 0.125;
        const float w2 = karisWeight(g2) * 0.125;
        const float w3 = karisWeight(g3) * 0.125;
        const float w4 = karisWeight(g4) * 0.125;

        result = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) /
                 max(w0 + w1 + w2 + w3 + w4, 0.0001);
        result = prefilter(result);
    } else {
        result = g0 * 0.5 + (g1 + g2 + g3 + g4) * 0.125;
    }

    outColor = vec4(result, 1.0);
}
