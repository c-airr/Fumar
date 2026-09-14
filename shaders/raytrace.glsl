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

/// A vertex, laid out exactly as fumar::Vertex in engine/render/include/
/// fumar/render/mesh.hpp. Nothing checks that they agree.
struct RayVertex {
    vec3 position;
    vec3 normal;
    vec2 uv;
};

// Two pointers, in the C sense. Given an address, these say how to read what is
// there - which is how a ray reaches the vertices of a mesh nobody bound.
layout(buffer_reference, scalar) readonly buffer VertexBuffer {
    RayVertex vertices[];
};
layout(buffer_reference, scalar) readonly buffer IndexBuffer {
    uint indices[];
};

/// What one instance in the scene is, and where its geometry lives.
struct InstanceRecord {
    uint64_t vertexAddress;
    uint64_t indexAddress;
    vec4 baseColor;
    float metallic;
    float roughness;

    /// Slot in sceneTextures, or kNoTexture for a flat colour.
    uint textureIndex;

    vec2 uvScale;
    float padding;
};

/// Matches kNoTexture in engine/render/include/fumar/render/renderer.hpp.
const uint kNoTexture = 0xFFFFFFFFu;

/// Must match kMaxSceneTextures in engine/render/src/renderer.cpp.
const int kMaxSceneTextures = 128;

/// Every texture in the scene, reachable without knowing in advance which one
/// is wanted.
///
/// The rasteriser binds ONE material before each draw, because it knows what it
/// is drawing. A ray does not: it finds out what it hit only after it has hit
/// it, so the only way to read that surface's texture is for all of them to be
/// reachable at once. Slots past the end of the scene hold the default texture,
/// so a stale index samples white rather than reading a descriptor nobody
/// wrote.
layout(set = 0, binding = 3) uniform sampler2D sceneTextures[kMaxSceneTextures];

/// Indexed by the custom index carried on each instance. This is the whole
/// reason a hit means anything: without it a ray reports a distance and a
/// triangle number, and no way at all to find out what it struck.
layout(set = 0, binding = 2, scalar) readonly buffer InstanceBuffer {
    InstanceRecord instanceRecords[];
};

/// How many rays each question is worth.
///
/// These are low for ray tracing, and they can be because the samples are
/// ARRANGED rather than scattered - see vogelDisc below. Sixteen random rays
/// look worse than eight evenly spread ones.
const int kShadowSamples = 8;
const int kOcclusionSamples = 12;

/// Bounced light is the expensive one: each sample is a full traversal looking
/// for the NEAREST surface, plus a shadow ray where it lands. Eight is enough
/// for light that is low-frequency by nature - what bounces is broad and soft,
/// so the noise left over is broad and soft too.
const int kIndirectSamples = 8;

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

/// What the scene looks like along a ray: the colour that comes back.
///
/// What the scene looks like along a ray: the radiance that comes back.
///
/// One bounce, and only one. The surface it lands on is lit by the sun and the
/// sky, with a shadow ray of its own, but nothing reflected IN that surface is
/// traced further - a mirror facing a mirror shows sky, not a corridor. Each
/// extra bounce multiplies the cost and, outside of a hall of mirrors, changes
/// very little.
///
/// `includeSunDisc` is the one thing that differs between its two callers. A
/// mirror should show the sun; a diffuse bounce should not, because the disc is
/// a handful of pixels of enormous brightness and a few hemisphere samples that
/// happen to land on it turn into white speckles that are not light, only
/// sampling error. The sun reaches a diffuse surface through the direct term,
/// which integrates the whole disc analytically.
vec3 traceScene(vec3 origin, vec3 rayDirection, float maxDistance, bool includeSunDisc) {
    rayQueryEXT query;

    // No TerminateOnFirstHit here, unlike a shadow ray: this one has to find the
    // NEAREST surface, not merely establish that something is in the way.
    rayQueryInitializeEXT(query, sceneStructure, gl_RayFlagsOpaqueEXT, 0xFF, origin, 0.01,
                          rayDirection, maxDistance);

    // The loop the shadow rays do not need. Traversal reports candidate hits
    // and the shader decides; with opaque geometry there is nothing to decide,
    // so this simply runs to completion.
    while (rayQueryProceedEXT(query)) {
    }

    if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
        // Nothing there: what comes back is the sky.
        return includeSunDisc ? skyWithSun(rayDirection) : skyRadiance(rayDirection);
    }

    const int instanceIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);
    const int primitiveIndex = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
    const vec2 barycentrics = rayQueryGetIntersectionBarycentricsEXT(query, true);
    const float distance = rayQueryGetIntersectionTEXT(query, true);

    const InstanceRecord record = instanceRecords[instanceIndex];

    VertexBuffer vertexBuffer = VertexBuffer(record.vertexAddress);
    IndexBuffer indexBuffer = IndexBuffer(record.indexAddress);

    // Three indices per triangle, and the primitive index counts triangles.
    const uint i0 = indexBuffer.indices[primitiveIndex * 3 + 0];
    const uint i1 = indexBuffer.indices[primitiveIndex * 3 + 1];
    const uint i2 = indexBuffer.indices[primitiveIndex * 3 + 2];

    // Barycentrics come back as two numbers; the third is what is left of one.
    // They weight the three corners, which is exactly how the rasteriser
    // interpolates - done here by hand because nothing rasterised this.
    const vec3 weights = vec3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x,
                              barycentrics.y);

    const vec3 localNormal = normalize(vertexBuffer.vertices[i0].normal * weights.x +
                                       vertexBuffer.vertices[i1].normal * weights.y +
                                       vertexBuffer.vertices[i2].normal * weights.z);

    // The same interpolation, for the texture coordinate. Nothing rasterised
    // this triangle, so nothing interpolated anything for us.
    const vec2 uv = vertexBuffer.vertices[i0].uv * weights.x +
                    vertexBuffer.vertices[i1].uv * weights.y +
                    vertexBuffer.vertices[i2].uv * weights.z;

    // Into world space. The 3x4 matrix the query returns drops the bottom row,
    // which a direction does not need anyway. This ignores non-uniform scale -
    // the inverse transpose would be correct - which shows only on a stretched
    // curved surface seen in a reflection.
    const mat4x3 objectToWorld = rayQueryGetIntersectionObjectToWorldEXT(query, true);
    vec3 normal = normalize(mat3(objectToWorld) * localNormal);

    // Two-sided: a ray can land on the back of a triangle, and a normal facing
    // away from it would light the surface from behind.
    if (dot(normal, rayDirection) > 0.0) {
        normal = -normal;
    }

    const vec3 hitPoint = origin + rayDirection * distance;

    vec3 albedo = record.baseColor.rgb;
    if (record.textureIndex != kNoTexture) {
        // nonuniformEXT is not decoration. Neighbouring rays hit different
        // objects, so this index varies within the wave; without it the
        // hardware is entitled to use one lane's index for all of them, and a
        // reflection shows the wrong object's texture in patches.
        //
        // textureLod with level 0 rather than texture(): there are no
        // derivatives here to pick a mip level from, because there is no
        // neighbouring fragment marching along the same surface. The cost is
        // aliasing in a reflection of something far away - a ray differential
        // is what would fix it properly.
        albedo *= textureLod(sceneTextures[nonuniformEXT(record.textureIndex)],
                             uv * record.uvScale, 0.0).rgb;
    }

    // Lambert rather than the full reflectance model. What is being computed is
    // a reflection of a surface, at whatever size that reflection appears on
    // screen - the specular highlight of a reflected object is not something
    // anyone can see, and it would cost as much as the direct shading does.
    const vec3 sun = normalize(frame.sunDirection.xyz);
    const float nDotL = max(dot(normal, sun), 0.0);

    vec3 lit = skyIrradiance(normal) * albedo;
    if (nDotL > 0.0) {
        const float shadow = anyHit(hitPoint + normal * 0.01, sun, 0.0, 2000.0) ? 0.0 : 1.0;
        lit += albedo * frame.sunColor.rgb * frame.sunIntensity * nDotL * shadow / 3.14159265;
    }

    return lit;
}

/// Kept for the reflection path, which always wants the sun in it.
vec3 traceReflection(vec3 origin, vec3 rayDirection) {
    return traceScene(origin, rayDirection, 400.0, true);
}

/// Light arriving at this point from everywhere except directly from a light:
/// off the sky where the sky is visible, off whatever is in the way where it is
/// not.
///
/// This REPLACES the analytic sky ambient and the occlusion term together,
/// rather than adding to them, and that is the point rather than an
/// optimisation. Those two were an approximation and a correction to it: assume
/// an unobstructed sky, then darken where something is in the way. Sampling the
/// hemisphere answers the actual question once - and where a surface is
/// blocked, it returns the light coming off whatever is blocking it instead of
/// simply less sky. That is the difference between a shadowed wall going grey
/// and a shadowed wall picking up the colour of the red floor beside it.
///
/// Returns a mean radiance, the same quantity skyIrradiance returns, so callers
/// multiply by albedo exactly as before. Cosine-weighted sampling is what makes
/// the plain average correct: the pdf and the cosine term in the integral
/// cancel, leaving albedo * mean(incoming).
vec3 indirectLight(vec3 position, vec3 normal) {
    const vec3 origin = offsetOrigin(position, normal);

    vec3 tangent;
    vec3 bitangent;
    orthonormalBasis(normal, tangent, bitangent);

    // Offset from the other sampling rotations, so the sets do not line up and
    // reinforce each other's pattern.
    const float rotation = hash13(position + vec3(91.0)) * 6.2831853;

    vec3 sum = vec3(0.0);
    for (int i = 0; i < kIndirectSamples; ++i) {
        // A disc point lifted onto the hemisphere gives a cosine-weighted
        // distribution: denser near the normal, which is where light matters
        // most, and exactly the weighting that makes the plain average right.
        const vec2 disc = vogelDisc(i, kIndirectSamples, rotation);
        const float height = sqrt(max(1.0 - dot(disc, disc), 0.0));
        const vec3 direction =
            normalize(tangent * disc.x + bitangent * disc.y + normal * height);

        sum += traceScene(origin, direction, 200.0, false);
    }

    return sum / float(kIndirectSamples);
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

vec3 traceReflection(vec3 origin, vec3 rayDirection) {
    // Without ray tracing the only thing that can be reflected is the sky, which
    // is exactly what the analytic version was already doing.
    return skyWithSun(rayDirection);
}

vec3 indirectLight(vec3 position, vec3 normal) {
    // Never called without ray tracing - frame.indirectStrength is forced to
    // zero there - but defined so the shading code compiles unchanged.
    return skyIrradiance(normal);
}

float ambientOcclusion(vec3 position, vec3 normal) {
    return 1.0;
}

#endif

#endif // FUMAR_RAYTRACE_GLSL
