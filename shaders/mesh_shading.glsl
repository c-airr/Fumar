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

// No #version here: this file is the BODY of two shaders, not a shader. Which
// one is being built is decided by whether FUMAR_RAY_QUERY is defined before
// the include - see mesh.frag and mesh_rq.frag. Both variants exist because
// tracing rays needs an extension not every GPU has, and a single shader cannot
// be half-compiled.

#include "sky.glsl"
#include "object.glsl"
#include "raytrace.glsl"

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

/// The whole reflectance model for one light direction, without the light's own
/// colour or brightness.
///
/// Pulled out into a function because the sun and every placed light want
/// exactly the same maths and differ only in where the light comes from and how
/// much of it arrives. What it returns is the fraction of incoming light sent
/// towards the eye, already multiplied by the cosine term - so a caller
/// multiplies by the light's radiance and adds.
vec3 evaluateBrdf(vec3 n, vec3 v, vec3 l, vec3 albedo, vec3 f0, float metallic, float roughness) {
    const vec3 h = normalize(v + l);

    const float nDotL = max(dot(n, l), 0.0);
    const float nDotV = max(dot(n, v), 1e-4);
    const float nDotH = max(dot(n, h), 0.0);
    const float vDotH = max(dot(v, h), 0.0);

    const vec3 fresnel = fresnelSchlick(vDotH, f0);
    const float distribution = distributionGGX(nDotH, roughness);
    const float geometry = geometrySmith(nDotV, nDotL, roughness);

    const vec3 specular = (distribution * geometry * fresnel) / max(4.0 * nDotV * nDotL, 1e-4);

    // Energy conservation: light reflected off the surface is not also
    // available to scatter around inside it, and a metal has no inside to
    // scatter in at all.
    const vec3 diffuseWeight = (vec3(1.0) - fresnel) * (1.0 - metallic);

    return (diffuseWeight * albedo / kPi + specular) * nDotL;
}

/// How much of a light survives the trip to a point `distance` away.
///
/// The inverse square is the physics. The windowing term is not: light does not
/// actually stop at any distance, but a renderer that took that literally would
/// test every lamp in a level against every surface. This shape reaches exactly
/// zero at the light's range instead of being cut off there, which is the
/// difference between a light fading out and a visible circle on the floor.
float distanceFalloff(float distance, float range) {
    const float ratio = clamp(distance / max(range, 0.0001), 0.0, 1.0);
    const float window = 1.0 - ratio * ratio * ratio * ratio;

    // +1 in the denominator keeps it finite at distance zero, where the inverse
    // square would divide by nothing.
    return (window * window) / (distance * distance + 1.0);
}

void main() {
    // Interpolating a normal across a triangle shortens it, so it has to be
    // renormalised per fragment or large faces go dark in the middle.
    const vec3 n = normalize(vNormal);
    const vec3 v = normalize(frame.cameraPosition.xyz - vWorldPosition);
    const vec3 l = normalize(frame.sunDirection.xyz);

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

    // Only needed outside the reflectance function to decide whether a shadow
    // ray is worth tracing at all.
    const float nDotL = max(dot(n, l), 0.0);
    const float nDotV = max(dot(n, v), 1e-4);

    // --- the sun ------------------------------------------------------------
    // How much of the sun this point can actually see. Without ray tracing this
    // is always 1 and nothing casts a shadow - which is what every image before
    // this looked like.
    const float visibility = sunVisibility(vWorldPosition, n, nDotL);

    const vec3 sunRadiance = frame.sunColor.rgb * frame.sunIntensity;
    vec3 lit = evaluateBrdf(n, v, l, albedo, f0, metallic, roughness) * sunRadiance * visibility;

    // --- placed lights --------------------------------------------------------
    for (int i = 0; i < frame.lightCount; ++i) {
        const SceneLight light = frame.lights[i];

        const vec3 toLight = light.positionRange.xyz - vWorldPosition;
        const float distance = length(toLight);
        if (distance > light.positionRange.w) {
            continue;
        }

        const vec3 lightDirection = toLight / max(distance, 0.0001);
        const float lightNDotL = max(dot(n, lightDirection), 0.0);
        if (lightNDotL <= 0.0) {
            continue;
        }

        float attenuation = distanceFalloff(distance, light.positionRange.w);

        // A spot is a point light with the beam narrowed. The cone term is the
        // angle from its axis, faded between the two cosines so the edge of the
        // beam is soft rather than a hard circle.
        if (light.shape.z > 0.5) {
            const float cosAngle = dot(-lightDirection, normalize(light.directionOuter.xyz));
            const float cone = clamp((cosAngle - light.directionOuter.w) /
                                         max(light.shape.x - light.directionOuter.w, 0.0001),
                                     0.0, 1.0);

            // Squared, because a linear fade across the cone edge still reads
            // as a line.
            attenuation *= cone * cone;
        }

        if (attenuation <= 0.0) {
            continue;
        }

        float shadow = 1.0;
        if (light.shape.w > 0.5) {
            shadow = lightVisibility(vWorldPosition, n, light.positionRange.xyz, light.shape.y,
                                     lightNDotL);
        }

        const vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.a * attenuation;
        lit += evaluateBrdf(n, v, lightDirection, albedo, f0, metallic, roughness) * radiance *
               shadow;
    }

    // --- the sky ------------------------------------------------------------
    // Without this, everything the sun does not reach is pure black - which is
    // what the inside of a shadow looks like on the moon, and nowhere else.
    // Ambient light arrives from the whole sky, so what blocks it is not one
    // direction but how enclosed the point is. That is what ambient occlusion
    // measures, and it is the difference between an object standing on the
    // ground and one hovering a millimetre above it.
    const float occlusion = ambientOcclusion(vWorldPosition, n);

    const vec3 irradiance = skyIrradiance(n);
    lit += irradiance * albedo * (1.0 - metallic) * occlusion;

    // A rough surface reflects a blurred sky. Lerping the reflection direction
    // towards the normal is a cheap stand-in for that blur: it is what a
    // prefiltered environment map does properly, without the map.
    const vec3 reflection = normalize(mix(reflect(-v, n), n, roughness * roughness));
    const vec3 ambientSpecular = skyRadiance(reflection) *
                                 fresnelAmbient(nDotV, f0, roughness);
    lit += ambientSpecular * occlusion;

    // Selection tint, mixed in rather than added, so a bright object cannot
    // blow out to white and a dark one still visibly changes. Kept subtle
    // because the wireframe outline carries most of the signal - this only has
    // to say which object the outline belongs to.
    lit = mix(lit, lit * kSelectionTint * 2.5, object.highlight * 0.35);

    outColor = vec4(lit, 1.0);
}
