#version 450

// Averages this frame with the ones before it, so sixteen rays per pixel add up
// to hundreds.
//
// Every traced term in this engine - shadows, occlusion, the bounced light -
// estimates an integral from a handful of rays, and a handful of rays is never
// the right answer: it is the right answer plus noise. Sixteen rays over a
// hemisphere leave visible grain, and the only honest way to remove it is more
// samples. Tracing 160 rays per pixel is not an option at 180 frames a second;
// tracing 16 and REUSING the last ten frames is the same number of samples for
// a sixteenth of the cost.
//
// That reuse is what this pass is. For each pixel it works out where the same
// point in the world was on the screen last frame, reads what was accumulated
// there, and mixes a little of the new estimate into it. Standing still, the
// picture converges within about a fifth of a second and the grain disappears.
//
// Three things make the difference between that and a smeared mess:
//
//   - Reprojection. The camera moved, so the pixel did not look at the same
//     thing last frame. The world position comes out of the depth buffer, and
//     last frame's camera matrix says where it was then. For static geometry
//     this is exact, not an approximation.
//
//   - Rejection. If what was at that spot last frame was a DIFFERENT surface -
//     something moved, or an edge uncovered what was behind it - its colour is
//     worthless. The distance to the camera is stored alongside the colour
//     precisely so this pass can ask "was this the same surface?" and throw the
//     history away when the answer is no.
//
//   - Clamping. Shading that depends on the view - a specular highlight, a
//     reflection - slides across a static surface as the camera moves, so even
//     a perfect reprojection reads a value that is no longer true. Clamping the
//     history into the range of colours actually present around this pixel
//     keeps a stale highlight from being dragged along behind the camera.
//
// Order matters: this runs on the HDR image before bloom and tone mapping, so
// what is averaged is scene radiance. Averaging after tone mapping would
// average through a curve, and the mean of a curve is not the curve of a mean.

#include "frame.glsl"

/// What the scene pass just produced: the right answer plus noise.
layout(set = 1, binding = 0) uniform sampler2D sceneCurrent;

/// What this pass produced last frame. rgb is accumulated radiance, and alpha
/// is the distance from the camera to the surface that was shaded - carried
/// along because the rejection test below has no other way to find out what the
/// stored colour belonged to.
layout(set = 1, binding = 1) uniform sampler2D sceneHistory;

/// This frame's depth, resolved to one sample per pixel. The only thing that
/// can turn a pixel back into a point in the world.
layout(set = 1, binding = 2) uniform sampler2D sceneDepth;

layout(location = 0) in vec2 vUV;

layout(location = 0) out vec4 outColor;

/// Never let a pixel stop taking new samples entirely.
///
/// At 0.02 the history is a 50-frame average, which is cleaner than anything a
/// real-time tracer can produce in one frame - and also a quarter of a second
/// behind the scene. Past that the lag costs more than the noise.
const float kMinBlend = 0.02;

/// How far outside the local range of colours a stored value may sit before it
/// is pulled back in, in standard deviations.
///
/// One is the textbook value and clamps aggressively enough to eat some of the
/// convergence: the grain this pass exists to remove IS local variation, so a
/// box drawn too tightly around it rejects exactly the history that was going
/// to average it away.
const float kClipSigma = 1.5;

/// How much the distance to a surface may differ from the stored one and still
/// count as the same surface, as a fraction of that distance.
///
/// Relative rather than absolute: at a metre away a centimetre is a different
/// surface, at a hundred metres it is the same one seen half a pixel over. The
/// stored value is a half float, worth about three decimal digits, so anything
/// below a percent would be rejecting rounding error.
const float kDepthTolerance = 0.05;

/// Luminance and two colour differences, instead of red, green and blue.
///
/// The clamp below bounds the history inside a box. In RGB that box has an
/// arbitrary orientation with respect to anything a viewer can see, so clipping
/// against it shifts hue: a value pulled back in red but not in green comes out
/// a different colour. In YCoCg one axis is brightness, which is the axis the
/// noise actually lives on, so the box is tight where it needs to be and the
/// hue survives.
vec3 toYCoCg(vec3 c) {
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                0.5 * c.r - 0.5 * c.b,
                -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

vec3 toRGB(vec3 c) {
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

/// Where a pixel of this frame's depth buffer is in the world.
vec3 worldFromDepth(vec2 uv, float depth) {
    // Depth goes in unchanged: fumar's projection puts the near plane at 0 and
    // the far plane at 1, which is Vulkan's convention and already what the
    // depth buffer holds. The OpenGL -1..1 remapping would be wrong here.
    const vec4 world = frame.invViewProjection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return world.xyz / world.w;
}

void main() {
    const float depth = texture(sceneDepth, vUV).r;
    const vec3 current = texture(sceneCurrent, vUV).rgb;

    // The far plane: nothing was drawn here, so this is sky. The sky is an
    // analytic function of a direction - there is no sampling in it and nothing
    // to converge - and a point at infinity has no screen position to reproject
    // to. Taken as it is, with a zero distance that marks it as "no surface"
    // for next frame's rejection test.
    if (depth >= 1.0) {
        outColor = vec4(current, 0.0);
        return;
    }

    const vec2 texel = 1.0 / vec2(textureSize(sceneCurrent, 0));

    // --- what colours are around this pixel right now -----------------------
    // The mean and the spread of the nine pixels centred here: the box the
    // history gets clamped into, and the fallback when there is no usable
    // history to clamp.
    vec3 sum = vec3(0.0);
    vec3 sumSquares = vec3(0.0);

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const vec3 neighbour = toYCoCg(texture(sceneCurrent, vUV + vec2(x, y) * texel).rgb);
            sum += neighbour;
            sumSquares += neighbour * neighbour;
        }
    }

    const vec3 mean = sum / 9.0;

    // max against zero because the two terms are nearly equal on a flat
    // surface, and floating point subtraction there can land just below it -
    // the square root of a negative number is a NaN that would spread.
    const vec3 sigma = sqrt(max(sumSquares / 9.0 - mean * mean, vec3(0.0)));

    // --- where this point was last frame ------------------------------------
    const vec3 world = worldFromDepth(vUV, depth);
    const vec4 previousClip = frame.prevViewProjection * vec4(world, 1.0);
    const vec2 previousUV = (previousClip.xy / previousClip.w) * 0.5 + 0.5;

    // Dividing by w is only meaningful in front of the camera: a point behind
    // it produces a plausible-looking coordinate out of a sign flip.
    const bool reprojected = previousClip.w > 0.0 &&
                             all(greaterThanEqual(previousUV, vec2(0.0))) &&
                             all(lessThanEqual(previousUV, vec2(1.0)));

    // Turned off, this pass is a copy. Not a 3x3 average, which is what the
    // branch below falls back to - trading the grain for a blur is a choice
    // worth making for the handful of pixels that have just been uncovered, and
    // not one to impose on the whole image when the filter is meant to be out
    // of the way entirely.
    vec3 accumulated = current;

    if (frame.temporalStrength > 0.0) {
        // No usable history: nine noisy samples averaged are nine times the
        // samples, blurred over a 3x3 area. Blurry beats speckled, and it lasts
        // only the few frames it takes the history to refill.
        accumulated = toRGB(mean);

        if (reprojected) {
            const vec4 history = texture(sceneHistory, previousUV);

            // How far this point WAS from the camera, which is what last frame
            // would have stored here if it was looking at the same thing.
            const float expected = length(world - frame.prevCameraPosition.xyz);

            const bool sameSurface = history.a > 0.0 &&
                                     abs(history.a - expected) <= kDepthTolerance * expected;

            // A single NaN in the history would be averaged into itself forever
            // and leave a permanent hole in the image. Cheaper to test for than
            // to track down later.
            const bool finite = !any(isnan(history.rgb)) && !any(isinf(history.rgb));

            if (sameSurface && finite) {
                const vec3 clamped = clamp(toYCoCg(history.rgb), mean - kClipSigma * sigma,
                                           mean + kClipSigma * sigma);

                // The exponential average. A fixed fraction of the new estimate
                // each frame, rather than a running per-pixel count, so the
                // filter has no state beyond the image itself - and a surface
                // that comes back into view starts converging immediately
                // instead of waiting on a counter nobody kept.
                const float blend = max(1.0 - frame.temporalStrength, kMinBlend);
                accumulated = mix(toRGB(clamped), current, blend);
            }
        }
    }

    // The distance, for next frame's rejection test. Not a spare channel being
    // borrowed: without it this pass cannot tell a reprojection that landed on
    // the same surface from one that landed on whatever was behind it.
    outColor = vec4(accumulated, length(world - frame.cameraPosition.xyz));
}
