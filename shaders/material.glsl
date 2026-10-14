// Materials, as the rasteriser and the rays both read them.
//
// One list of records and one array of textures for the whole scene, in the
// frame's descriptor set. A draw says which record is its material by index; a
// ray finds out what it hit and reads the same record by the same index. That
// is the point of the arrangement: a surface seen in a reflection is the
// surface the camera sees, not an approximation kept for the rays.

#ifndef FUMAR_MATERIAL_GLSL
#define FUMAR_MATERIAL_GLSL

/// The texture array's size: 1024 (kMaxSceneTextures in engine/render/include/
/// fumar/render/material_gpu.hpp) unless the GPU's descriptor limits allow
/// fewer, in which case the renderer sets it to what they allow when it builds
/// the pipeline. A specialization constant, because the descriptor layout is
/// sized at run time and the shader's array has to agree with it.
layout(constant_id = 0) const int kMaxSceneTextures = 1024;

/// One material. Laid out exactly as fumar::MaterialRecord in
/// engine/render/include/fumar/render/material_gpu.hpp, whose offsets are
/// static_assert'ed there - 80 bytes, std430-clean so GPUs without scalar block
/// layout read it the same way.
struct MaterialRecord {
    vec4 baseColorFactor;            // offset 0
    vec3 emissive;                   // 16: colour * intensity
    float metallic;                  // 28
    float roughness;                 // 32
    float normalScale;               // 36
    float occlusionStrength;         // 40
    float alphaCutoff;               // 44: negative = opaque
    vec2 uvScale;                    // 48
    uint baseColorTexture;           // 56
    uint metallicRoughnessTexture;   // 60
    uint normalTexture;              // 64
    uint occlusionTexture;           // 68
    uint emissiveTexture;            // 72
    uint padding;                    // 76
};

/// Every texture in the scene, reachable by index.
///
/// A ray finds out what it hit only after it has hit it, so the only way to
/// read that surface's texture is for all of them to be reachable at once; the
/// rasteriser now reads its maps the same way. Slots past the end of the scene
/// hold a white texture, so nothing ever reads a descriptor nobody wrote.
layout(set = 0, binding = 3) uniform sampler2D sceneTextures[kMaxSceneTextures];

layout(set = 0, binding = 4, std430) readonly buffer MaterialBuffer {
    MaterialRecord materials[];
};

/// What a material says about one point of a surface.
struct MaterialSample {
    vec3 albedo;
    float alpha;
    vec3 normal;       // world space, with the normal map applied
    float roughness;
    float metallic;
    float occlusion;   // 1 = open, 0 = fully occluded; already scaled by strength
    vec3 emissive;     // radiance given off
};

/// The surface's own normal bent by its normal map.
///
/// The map stores directions in tangent space - x along U, y along V, z out of
/// the surface - so it is turned into the world through the frame the mesh
/// carries: tangent, bitangent (cross(n, t) * w, w being the handedness), and
/// normal. The tangent is squared against the normal first, because
/// interpolating across a triangle leaves the two slightly off perpendicular,
/// and a frame that leans tilts every texel with it.
vec3 applyNormalMap(vec3 n, vec4 tangent, vec3 mapped, float scale) {
    vec3 t = tangent.xyz - n * dot(n, tangent.xyz);
    if (dot(t, t) < 1e-10) {
        // No frame to read the map against - a mesh without UVs. The plain
        // normal beats a NaN.
        return n;
    }
    t = normalize(t);
    const vec3 b = cross(n, t) * (tangent.w < 0.0 ? -1.0 : 1.0);
    return normalize(t * (mapped.x * scale) + b * (mapped.y * scale) + n * mapped.z);
}

/// The full sample, for the rasteriser.
///
/// The index is the same for every fragment of a draw, so the textures are
/// indexed without nonuniformEXT and with derivatives - texture() picks the mip
/// level from how fast the UVs change across the screen.
MaterialSample sampleMaterial(uint index, vec2 uv, vec3 n, vec4 tangent) {
    const MaterialRecord m = materials[index];
    const vec2 st = uv * m.uvScale;

    const vec4 base = texture(sceneTextures[m.baseColorTexture], st) * m.baseColorFactor;
    const vec4 metalRough = texture(sceneTextures[m.metallicRoughnessTexture], st);
    const vec3 mapped = texture(sceneTextures[m.normalTexture], st).xyz * 2.0 - 1.0;
    const float occlusion = texture(sceneTextures[m.occlusionTexture], st).r;
    const vec3 emission = texture(sceneTextures[m.emissiveTexture], st).rgb;

    MaterialSample s;
    s.albedo = base.rgb;
    s.alpha = base.a;
    s.normal = applyNormalMap(n, tangent, mapped, m.normalScale);
    // glTF's layout: roughness in green, metalness in blue.
    s.roughness = m.roughness * metalRough.g;
    s.metallic = m.metallic * metalRough.b;
    s.occlusion = mix(1.0, occlusion, m.occlusionStrength);
    s.emissive = m.emissive * emission;
    return s;
}

#ifdef FUMAR_RAY_QUERY

/// The sample a ray needs where it lands: no normal map - in a reflection or a
/// bounce of light it is not visible, and a frame per hit is not free - and
/// textureLod at level 0, because a ray has no neighbouring fragments to take a
/// mip level from. nonuniformEXT throughout: neighbouring rays hit different
/// objects, and without it the hardware may use one lane's index for all.
MaterialSample sampleMaterialHit(uint index, vec2 uv, vec3 n) {
    const MaterialRecord m = materials[nonuniformEXT(index)];
    const vec2 st = uv * m.uvScale;

    const vec4 base = textureLod(sceneTextures[nonuniformEXT(m.baseColorTexture)], st, 0.0) * m.baseColorFactor;
    const vec4 metalRough = textureLod(sceneTextures[nonuniformEXT(m.metallicRoughnessTexture)], st, 0.0);
    const vec3 emission = textureLod(sceneTextures[nonuniformEXT(m.emissiveTexture)], st, 0.0).rgb;

    MaterialSample s;
    s.albedo = base.rgb;
    s.alpha = base.a;
    s.normal = n;
    s.roughness = m.roughness * metalRough.g;
    s.metallic = m.metallic * metalRough.b;
    s.occlusion = 1.0;
    s.emissive = m.emissive * emission;
    return s;
}

#endif

#endif // FUMAR_MATERIAL_GLSL
