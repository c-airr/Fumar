#version 450

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vWorldPosition;

// Set 1 is per-material. Splitting it from set 0 means switching material does
// not disturb the camera binding.
layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;

layout(push_constant) uniform PushConstants {
    mat4 model;
    vec4 baseColor;
    float highlight;
} object;

layout(location = 0) out vec4 outColor;

const vec3 kLightDirection = normalize(vec3(0.45, 0.85, 0.35));
const vec3 kLightColor = vec3(1.0, 0.97, 0.92);
const vec3 kSkyColor = vec3(0.34, 0.38, 0.46);

// The editor accent, matched to the interface so selection reads as one idea
// across the whole application.
const vec3 kSelectionTint = vec3(0.659, 0.373, 0.173);

void main() {
    // Interpolating a normal across a triangle shortens it, so it has to be
    // renormalised per fragment or large faces go dark in the middle.
    vec3 normal = normalize(vNormal);

    // Lambert: brightness falls off with the cosine of the angle between the
    // surface and the light, which is what a dot product of two unit vectors
    // gives directly.
    float diffuse = max(dot(normal, kLightDirection), 0.0);

    // Cheap stand-in for bounced light: surfaces facing up pick up more sky.
    // Without something like this, everything in shadow is pure black.
    float skyAmount = 0.5 + 0.5 * normal.y;
    vec3 ambient = kSkyColor * skyAmount * 0.4;

    // The texture is multiplied by the material colour, so an untextured
    // material uses a 1x1 white texture and shows its colour unchanged.
    vec3 albedo = texture(baseColorTexture, vUV).rgb * object.baseColor.rgb;
    vec3 lit = albedo * (ambient + kLightColor * diffuse);

    // Selection tint, mixed in rather than added, so a bright object cannot
    // blow out to white and a dark one still visibly changes.
    lit = mix(lit, kSelectionTint, object.highlight * 0.55);

    // A rim term makes the selected object read at a glance even when it is
    // facing away from the light.
    if (object.highlight > 0.0) {
        vec3 toCamera = normalize(-vWorldPosition);
        float rim = pow(1.0 - abs(dot(normal, toCamera)), 2.0);
        lit += kSelectionTint * rim * object.highlight * 0.8;
    }

    outColor = vec4(lit, 1.0);
}
