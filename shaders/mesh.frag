#version 450

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vWorldPosition;

// Set 1 is per-material. Splitting it from set 0 means switching material does
// not disturb the camera binding.
layout(set = 1, binding = 0) uniform sampler2D baseColor;

layout(location = 0) out vec4 outColor;

const vec3 kLightDirection = normalize(vec3(0.45, 0.85, 0.35));
const vec3 kLightColor = vec3(1.0, 0.97, 0.92);
const vec3 kSkyColor = vec3(0.30, 0.36, 0.48);

void main() {
    // Interpolating a normal across a triangle shortens it, so it has to be
    // renormalised per fragment or the lighting comes out too dark in the
    // middle of large faces.
    vec3 normal = normalize(vNormal);

    // Lambert: brightness falls off with the cosine of the angle between the
    // surface and the light, which is exactly what a dot product of two unit
    // vectors gives.
    float diffuse = max(dot(normal, kLightDirection), 0.0);

    // Cheap stand-in for bounced light: surfaces facing up pick up more sky.
    // Without something like this, everything in shadow is pure black.
    float skyAmount = 0.5 + 0.5 * normal.y;
    vec3 ambient = kSkyColor * skyAmount * 0.35;

    vec3 albedo = texture(baseColor, vUV).rgb;
    vec3 lit = albedo * (ambient + kLightColor * diffuse);

    outColor = vec4(lit, 1.0);
}
