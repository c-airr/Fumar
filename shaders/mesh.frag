#version 450

// Surface shading: Cook-Torrance, the model glTF describes its materials in and
// that every renderer of the last decade uses.
//
// The idea behind it is that a surface is not smooth but covered in microscopic
// facets, each a perfect mirror. Roughness says how wildly they are tilted. All
// aligned and the reflection is sharp; scattered and it smears into a broad
// sheen. Three terms answer three questions about those facets: how many face
// the right way to send light at the eye (D), how many are blocked by their
// neighbours (G), and how much light a facet reflects at this angle rather than
// letting through (F).

#include "sky.glsl"
#include "object.glsl"

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vWorldPosition;

// Set 1 is per-material. Splitting it from set 0 means switching material does
// not disturb the frame binding.
layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;

layout(location = 0) out vec4 outColor;

// The editor accent, matched to the interface so selection reads as one idea
// across the whole application.
const vec3 kSelectionTint = vec3(0.659, 0.373, 0.173);

const float kPi = 3.14159265359;

/// D - the fraction of facets aligned to reflect from the light to the eye.
///
/// GGX (Trowbridge-Reitz). Its long tail is what makes rough metal look right:
/// a highlight that fades out gradually instead of ending abruptly.
float distributionGGX(float nDotH, float roughness) {
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(kPi * d * d, 1e-7);
}

/// G - how much of that reflection actually escapes.
///
/// On a rough surface a facet can be hidden behind another from the light's
/// side or the eye's, so the same term is evaluated twice and multiplied.
/// Without it, grazing angles gain energy from nowhere and edges glow white.
float geometrySmith(float nDotV, float nDotL, float roughness) {
    const float r = roughness + 1.0;
    const float k = (r * r) / 8.0;
    const float gv = nDotV / (nDotV * (1.0 - k) + k);
    const float gl = nDotL / (nDotL * (1.0 - k) + k);
    return gv * gl;
}

/// F - Fresnel: everything becomes a mirror at a glancing angle.
///
/// Look straight down at water and you see the bottom; look along it and you
/// see the sky. Schlick's approximation of that, which needs only the
/// reflectance head-on (F0) and the angle.
vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

/// The same, widened for ambient light, which arrives from every direction at
/// once rather than from one. Clamping against roughness stops a rough surface
/// from picking up a mirror-bright rim it has no facets to produce.
vec3 fresnelAmbient(float cosTheta, vec3 f0, float roughness) {
    const vec3 ceiling = max(vec3(1.0 - roughness), f0);
    return f0 + (ceiling - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    // Interpolating a normal across a triangle shortens it, so it has to be
    // renormalised per fragment or large faces go dark in the middle.
    const vec3 n = normalize(vNormal);
    const vec3 v = normalize(frame.cameraPosition.xyz - vWorldPosition);
    const vec3 l = normalize(frame.sunDirection.xyz);
    const vec3 h = normalize(v + l);

    // The texture is multiplied by the material colour, so an untextured
    // material uses a 1x1 white texture and shows its colour unchanged.
    const vec3 albedo = texture(baseColorTexture, vUV).rgb * object.baseColor.rgb;

    const float metallic = clamp(object.metallic, 0.0, 1.0);

    // Never exactly zero: a perfectly smooth surface makes the GGX lobe a
    // spike narrower than a pixel, which turns into flickering white dots.
    const float roughness = clamp(object.roughness, 0.045, 1.0);

    // What the surface reflects head-on. Dielectrics reflect about 4% of the
    // light and let the rest through to be coloured by the material underneath;
    // metals reflect nearly everything and tint it, which is why a gold mirror
    // image is gold. One number, two completely different materials.
    const vec3 f0 = mix(vec3(0.04), albedo, metallic);

    const float nDotL = max(dot(n, l), 0.0);
    const float nDotV = max(dot(n, v), 1e-4);
    const float nDotH = max(dot(n, h), 0.0);
    const float vDotH = max(dot(v, h), 0.0);

    // --- the sun ------------------------------------------------------------
    const vec3 fresnel = fresnelSchlick(vDotH, f0);
    const float distribution = distributionGGX(nDotH, roughness);
    const float geometry = geometrySmith(nDotV, nDotL, roughness);

    const vec3 specular = (distribution * geometry * fresnel) /
                          max(4.0 * nDotV * nDotL, 1e-4);

    // Energy conservation: light reflected off the surface is not also
    // available to scatter around inside it, and a metal has no inside to
    // scatter in at all.
    const vec3 diffuseWeight = (vec3(1.0) - fresnel) * (1.0 - metallic);

    const vec3 sunRadiance = frame.sunColor.rgb * frame.sunIntensity;
    vec3 lit = (diffuseWeight * albedo / kPi + specular) * sunRadiance * nDotL;

    // --- the sky ------------------------------------------------------------
    // Without this, everything the sun does not reach is pure black - which is
    // what the inside of a shadow looks like on the moon, and nowhere else.
    const vec3 irradiance = skyIrradiance(n);
    lit += irradiance * albedo * (1.0 - metallic);

    // A rough surface reflects a blurred sky. Lerping the reflection direction
    // towards the normal is a cheap stand-in for that blur: it is what a
    // prefiltered environment map does properly, without the map.
    const vec3 reflection = normalize(mix(reflect(-v, n), n, roughness * roughness));
    const vec3 ambientSpecular = skyRadiance(reflection) *
                                 fresnelAmbient(nDotV, f0, roughness);
    lit += ambientSpecular;

    // Selection tint, mixed in rather than added, so a bright object cannot
    // blow out to white and a dark one still visibly changes. Kept subtle
    // because the wireframe outline carries most of the signal - this only has
    // to say which object the outline belongs to.
    lit = mix(lit, lit * kSelectionTint * 2.5, object.highlight * 0.35);

    outColor = vec4(lit, 1.0);
}
