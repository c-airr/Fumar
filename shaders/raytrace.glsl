// Visibility queries, answered by tracing rays when the hardware can.
//
// Both functions have a definition for each case. Without ray tracing they
// return "fully visible" and "not occluded", which is precisely the assumption
// a renderer without shadows is making anyway - it just never says so out loud.
// Everything that calls them is written once and does not branch.

#ifndef FUMAR_RAYTRACE_GLSL
#define FUMAR_RAYTRACE_GLSL

#include "frame.glsl"

#ifdef FUMAR_RAY_QUERY

// The whole scene, as a tree of bounding volumes over every triangle in it.
// Rebuilt each frame, because objects move.
layout(set = 0, binding = 1) uniform accelerationStructureEXT sceneStructure;

/// How many rays each question is worth.
///
/// These are low for ray tracing, and they can be because the samples are
/// ARRANGED rather than scattered - see vogelDisc below. Sixteen random rays
/// look worse than eight evenly spread ones.
const int kShadowSamples = 8;
const int kOcclusionSamples = 12;

/// One pseudo-random number from a point in the WORLD.
///
/// Not from gl_FragCoord, and that distinction is the whole reason this
/// function takes a position. Seeded from the pixel, the sampling pattern
/// belongs to the screen: turn the camera and every surface slides underneath a
/// noise pattern that stays where it is, so the speckle along a shadow edge
/// appears to crawl across the objects rather than sit on them. Seeded from the
/// surface point, the pattern is glued to the geometry and moves with it - the
/// grain that is left reads as texture on the object instead of as dirt on the
/// lens.
///
/// The position is scaled up first. Neighbouring pixels are only thousandths of
/// a unit apart on a nearby surface, and a hash fed inputs that close returns
/// values that are close, which turns fine grain into slow blotches.
float hash13(vec3 position) {
    vec3 p = fract(position * 137.13 * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

/// Evenly spaced points on a disc, from a Vogel spiral.
///
/// This replaces random sampling and is most of why the noise went away. Random
/// points clump: with eight of them, some regions of the sun's disc get three
/// samples and others none, and that imbalance IS the speckle. A Vogel spiral -
/// each point turned by the golden angle from the last - spreads them as evenly
/// as points can be spread, and the only randomness left is one rotation for
/// the whole set, which decorrelates neighbouring pixels without unbalancing
/// any of them.
///
/// The same eight rays, arranged rather than scattered.
vec2 vogelDisc(int index, int count, float rotation) {
    // sqrt keeps the points evenly spread by AREA rather than by radius - a
    // disc has more room further out, so without it they crowd the centre.
    const float radius = sqrt((float(index) + 0.5) / float(count));

    // 2.39996 radians: the golden angle, the turn that never lines points up
    // into spokes however many of them there are.
    const float theta = float(index) * 2.39996323 + rotation;
    return vec2(cos(theta), sin(theta)) * radius;
}

/// Any pair of axes perpendicular to n. Which pair does not matter, only that
/// they are orthonormal - the near-degenerate case is picking a reference axis
/// parallel to n, which is what the branch avoids.
void orthonormalBasis(vec3 n, out vec3 t, out vec3 b) {
    t = normalize(abs(n.y) < 0.99 ? cross(vec3(0.0, 1.0, 0.0), n) : cross(vec3(1.0, 0.0, 0.0), n));
    b = cross(n, t);
}

/// True if anything at all lies between origin and distance along direction.
///
/// Three flags earn their place here. TerminateOnFirstHit says we are asking
/// whether ANYTHING is in the way, not what is nearest, so the traversal can
/// stop the moment it finds something. Opaque skips any-hit shaders we do not
/// have. SkipClosestHitShader says not to look for one either.
bool anyHit(vec3 origin, vec3 direction, float minDistance, float maxDistance) {
    rayQueryEXT query;
    rayQueryInitializeEXT(query, sceneStructure,
                          gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT |
                              gl_RayFlagsSkipClosestHitShaderEXT,
                          0xFF, origin, minDistance, direction, maxDistance);

    // With TerminateOnFirstHit the traversal finishes in one step, so this is
    // an `if`, not the `while` a full traversal would need.
    rayQueryProceedEXT(query);

    return rayQueryGetIntersectionTypeEXT(query, true) !=
           gl_RayQueryCommittedIntersectionNoneEXT;
}

/// Pushes a ray's start point off the surface it came from.
///
/// A ray leaving a triangle at exactly that triangle's position will find it
/// again: the intersection maths cannot tell "on the surface" from "just past
/// it" in floating point. The result is the classic acne of dark speckles over
/// every lit surface. Scaling the offset with distance to the camera matters
/// because float precision is absolute, not relative - a bias that works up
/// close is invisible at range.
vec3 offsetOrigin(vec3 position, vec3 normal) {
    const float distance = length(position - frame.cameraPosition.xyz);
    return position + normal * (0.004 + distance * 0.0012);
}

/// How much of the sun's disc is visible from this point: 0 in full shadow, 1
/// in full light, in between along the penumbra.
///
/// The sun is not a point - it is a disc a quarter of a degree across, which is
/// exactly why real shadows have soft edges that get softer the further they
/// fall from whatever casts them. Sampling directions across that disc
/// reproduces it for free; a shadow map has to fake the same effect with a
/// blur that has no idea how far away the caster is.
float sunVisibility(vec3 position, vec3 normal, float nDotL) {
    // A surface facing away from the sun is already unlit by the cosine term.
    // No ray can change that, so tracing one would be pure cost.
    if (nDotL <= 0.0 || frame.shadowStrength <= 0.0) {
        return 1.0;
    }

    const vec3 origin = offsetOrigin(position, normal);
    const vec3 sun = normalize(frame.sunDirection.xyz);

    vec3 tangent;
    vec3 bitangent;
    orthonormalBasis(sun, tangent, bitangent);

    // tan() rather than the angle itself: the offset is applied at unit
    // distance along the ray, so the angle it subtends is its arctangent.
    const float spread = tan(max(frame.sunAngularRadius, 0.0001));

    // One random number for the whole set: which way the spiral is turned.
    const float rotation = hash13(position) * 6.2831853;

    float visible = 0.0;
    for (int i = 0; i < kShadowSamples; ++i) {
        const vec2 offset = vogelDisc(i, kShadowSamples, rotation) * spread;
        const vec3 direction = normalize(sun + tangent * offset.x + bitangent * offset.y);

        // A large finite distance rather than infinity: the sun is outside the
        // scene, so anything the ray can reach is a caster.
        visible += anyHit(origin, direction, 0.0, 2000.0) ? 0.0 : 1.0;
    }

    visible /= float(kShadowSamples);
    return mix(1.0, visible, frame.shadowStrength);
}

/// How much of a placed light reaches this point.
///
/// Same idea as the sun, with one difference that matters: the ray stops AT the
/// light rather than carrying on to infinity. Something behind the lamp is not
/// between you and it.
float lightVisibility(vec3 position, vec3 normal, vec3 lightPosition, float sourceRadius,
                      float nDotL) {
    if (nDotL <= 0.0 || frame.shadowStrength <= 0.0) {
        return 1.0;
    }

    const vec3 origin = offsetOrigin(position, normal);
    const vec3 toLight = lightPosition - origin;
    const float distance = length(toLight);
    if (distance <= 0.0001) {
        return 1.0;
    }

    const vec3 axis = toLight / distance;

    vec3 tangent;
    vec3 bitangent;
    orthonormalBasis(axis, tangent, bitangent);

    const float rotation = hash13(position + vec3(43.0)) * 6.2831853;

    // Fewer rays than the sun gets. A lamp lights a small part of the frame, so
    // the same budget spread over every light in the scene would cost far more
    // for far less of the picture.
    const int samples = 4;

    float visible = 0.0;
    for (int i = 0; i < samples; ++i) {
        const vec2 offset = vogelDisc(i, samples, rotation) * sourceRadius;
        const vec3 target = lightPosition + tangent * offset.x + bitangent * offset.y;
        const vec3 direction = normalize(target - origin);

        // Stopping just short of the light itself, so the lamp's own geometry -
        // if somebody models one - does not shadow it.
        visible += anyHit(origin, direction, 0.0, distance * 0.999) ? 0.0 : 1.0;
    }

    return mix(1.0, visible / float(samples), frame.shadowStrength);
}

/// How open the sky is above this point, sampled over a short distance.
///
/// The ambient term assumes light arrives from the entire sky. Under a table,
/// in a corner, or where two objects meet, most of that sky is blocked by
/// something nearby, and without accounting for it those places are lit as
/// brightly as open ground. The rays are short on purpose: this is about
/// contact and creases, not about distant geometry, which the sun's own shadow
/// already handles.
float ambientOcclusion(vec3 position, vec3 normal) {
    if (frame.occlusionStrength <= 0.0 || frame.occlusionRadius <= 0.0) {
        return 1.0;
    }

    const vec3 origin = offsetOrigin(position, normal);

    vec3 tangent;
    vec3 bitangent;
    orthonormalBasis(normal, tangent, bitangent);

    // Offset from the shadow rays' rotation, so the two sets do not line up and
    // reinforce each other's pattern.
    const float rotation = hash13(position + vec3(17.0)) * 6.2831853;

    float open = 0.0;
    for (int i = 0; i < kOcclusionSamples; ++i) {
        // A disc point lifted onto the hemisphere. Projecting an evenly spread
        // disc this way gives a COSINE-weighted distribution: denser near the
        // normal, which is where light matters most, and exactly the weighting
        // that makes the plain average of the results the right answer.
        const vec2 disc = vogelDisc(i, kOcclusionSamples, rotation);
        const float height = sqrt(max(1.0 - dot(disc, disc), 0.0));

        const vec3 direction =
            normalize(tangent * disc.x + bitangent * disc.y + normal * height);

        open += anyHit(origin, direction, 0.0, frame.occlusionRadius) ? 0.0 : 1.0;
    }

    open /= float(kOcclusionSamples);
    return mix(1.0, open, frame.occlusionStrength);
}

#else // no ray tracing

float sunVisibility(vec3 position, vec3 normal, float nDotL) {
    return 1.0;
}

float lightVisibility(vec3 position, vec3 normal, vec3 lightPosition, float sourceRadius,
                      float nDotL) {
    return 1.0;
}

float ambientOcclusion(vec3 position, vec3 normal) {
    return 1.0;
}

#endif

#endif // FUMAR_RAYTRACE_GLSL
