#version 450

// Vertex attributes, fed from the vertex buffer. The locations match
// Vertex::attributes() in engine/render/src/mesh.cpp - nothing validates that
// pairing, so it has to be kept in step by hand.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

// Set 0 holds everything constant for the whole frame. Keeping it in its own
// set means it can be bound once per frame instead of once per object.
layout(set = 0, binding = 0) uniform CameraData {
    mat4 view;
    mat4 projection;
    vec4 position;
} camera;

// Per-object data small enough to live in the command buffer itself.
layout(push_constant) uniform PushConstants {
    mat4 model;
} object;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vWorldPosition;

void main() {
    vec4 worldPosition = object.model * vec4(inPosition, 1.0);

    // Right to left: model space -> world space -> view space -> clip space.
    gl_Position = camera.projection * camera.view * worldPosition;

    // mat3() drops the translation, which is what a direction wants. This is
    // only correct for uniform scaling; non-uniform scale needs the inverse
    // transpose, or normals come out skewed.
    vNormal = mat3(object.model) * inNormal;

    vUV = inUV;
    vWorldPosition = worldPosition.xyz;
}
