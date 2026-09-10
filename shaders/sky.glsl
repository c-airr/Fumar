// The sky, as a function you can evaluate in any direction.
//
// Written this way on purpose: the background pass asks it what is behind each
// pixel, and the surface shader asks it what light is arriving from above. Both
// get the same answer, so a surface always looks like it belongs to the sky it
// is standing under. Load a cube map instead and the two would have to be kept
// in step by hand.
//
// This is a hand-tuned gradient, not an atmospheric simulation. A physically
// based model (Preetham, Hosek-Wilkie) is a page of fitted constants that only
// makes sense as a whole; here every value means something you can see.

#ifndef FUMAR_SKY_GLSL
#define FUMAR_SKY_GLSL

#include "frame.glsl"

/// The sky WITHOUT the sun in it: gradient plus a broad haze around the sun.
///
/// This is the version surfaces reflect. The sun's disc is tiny and extremely
/// bright, so a reflection that sampled it would either miss it entirely or hit
/// it and produce one blinding pixel that flickers as the camera moves - all
/// the aliasing, none of the light. The direct lighting term below accounts for
/// the sun properly instead.
vec3 skyRadiance(vec3 direction) {
    const float height = direction.y;

    // pow() below 1 pushes the horizon colour further up the sky, which is what
    // a real sky does - the gradient is not linear in angle.
    const float t = pow(clamp(height, 0.0, 1.0), 0.45);
    const vec3 sky = mix(frame.skyHorizonColor.rgb, frame.skyZenithColor.rgb, t);

    // Below the horizon there is ground rather than sky. The narrow smoothstep
    // is what keeps the seam from being a hard line on a distant floor.
    const vec3 base = mix(frame.groundColor.rgb, sky, smoothstep(-0.03, 0.03, height));

    // Air scatters sunlight forward, so the sky is brightest around the sun and
    // fades with angle. Two lobes: a tight one for the glare close in, a wide
    // one for the general brightening of that half of the sky.
    const float cosAngle = max(dot(direction, frame.sunDirection.xyz), 0.0);
    const float haze = pow(cosAngle, 48.0) * 0.55 + pow(cosAngle, 6.0) * 0.12;

    return base * frame.skyIntensity +
           frame.sunColor.rgb * haze * frame.sunIntensity * 0.12;
}

/// The sky WITH the sun disc, for the background pass.
vec3 skyWithSun(vec3 direction) {
    vec3 colour = skyRadiance(direction);

    const float cosAngle = dot(direction, frame.sunDirection.xyz);

    // The disc edge, in cosine space. cos() decreases as the angle grows, so
    // the outer edge is the SMALLER value and belongs first in smoothstep.
    const float inner = cos(frame.sunAngularRadius);
    const float outer = cos(frame.sunAngularRadius * 2.0);
    const float disc = smoothstep(outer, inner, cosAngle);

    // Far brighter than anything else in the frame, which is the point of
    // rendering in HDR: the tone mapper decides how that reads on screen
    // instead of the value being clamped to white on the way in.
    return colour + frame.sunColor.rgb * frame.sunIntensity * disc * 40.0;
}

/// Average light arriving at a surface facing `normal`, from the whole sky.
///
/// Properly this is an integral of the sky over the hemisphere around the
/// normal. Approximated here by blending between what an upward-facing surface
/// sees (sky) and what a downward-facing one sees (bounce off the ground) -
/// two evaluations instead of hundreds, and on a smooth gradient the difference
/// is not visible.
vec3 skyIrradiance(vec3 normal) {
    const float up = normal.y * 0.5 + 0.5;
    const vec3 sky = mix(frame.skyHorizonColor.rgb, frame.skyZenithColor.rgb, 0.35);
    return mix(frame.groundColor.rgb, sky, up) * frame.skyIntensity;
}

#endif // FUMAR_SKY_GLSL
