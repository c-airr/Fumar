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

/// How many rays are spent on each. Four is enough that a soft edge reads as an
/// edge rather than as noise, and cheap enough not to matter: at this
/// resolution it is a few million rays a frame, which is a fraction of what the
/// hardware is built for.
const int kShadowSamples = 8;
const int kOcclusionSamples = 16;

/// One pseudo-random number from a pixel coordinate.
///
/// Deliberately a function of position only, with nothing from the frame
/// counter in it: the noise pattern is then identical every frame, so what is
/// left reads as texture rather than as flicker. Mixing time in would look
/// better only with temporal accumulation to average it out.
float hash12(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

/// Two of them, decorrelated. Using hash12 twice with nearby inputs gives
/// values that are related, which shows up as structure in the sampling.
vec2 hash22(vec2 p) {
    return vec2(hash12(p), hash12(p + vec2(37.13, 71.79)));
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

    float visible = 0.0;
    for (int i = 0; i < kShadowSamples; ++i) {
        const vec2 rnd = hash22(gl_FragCoord.xy + vec2(float(i) * 17.0, float(i) * 29.0));

        // sqrt on the radius is what spreads points evenly over the disc. Using
        // the raw value clusters them in the middle, because a disc has more
        // area further out.
        const float angle = rnd.x * 6.2831853;
        const float radius = spread * sqrt(rnd.y);
        const vec3 direction =
            normalize(sun + (tangent * cos(angle) + bitangent * sin(angle)) * radius);

        // A large finite distance rather than infinity: the sun is outside the
        // scene, so anything the ray can reach is a caster.
        visible += anyHit(origin, direction, 0.0, 2000.0) ? 0.0 : 1.0;
    }

    visible /= float(kShadowSamples);
    return mix(1.0, visible, frame.shadowStrength);
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

    float open = 0.0;
    for (int i = 0; i < kOcclusionSamples; ++i) {
        const vec2 rnd = hash22(gl_FragCoord.xy + vec2(float(i) * 11.0, float(i) * 23.0));

        // Cosine-weighted: more samples where the surface receives more light,
        // which is what makes the average of the results the right answer
        // without weighting each one afterwards.
        const float angle = rnd.x * 6.2831853;
        const float radius = sqrt(rnd.y);
        const float height = sqrt(max(1.0 - rnd.y, 0.0));

        const vec3 direction =
            normalize(tangent * cos(angle) * radius + bitangent * sin(angle) * radius +
                      normal * height);

        open += anyHit(origin, direction, 0.0, frame.occlusionRadius) ? 0.0 : 1.0;
    }

    open /= float(kOcclusionSamples);
    return mix(1.0, open, frame.occlusionStrength);
}

#else // no ray tracing

float sunVisibility(vec3 position, vec3 normal, float nDotL) {
    return 1.0;
}

float ambientOcclusion(vec3 position, vec3 normal) {
    return 1.0;
}

#endif

#endif // FUMAR_RAYTRACE_GLSL
