#version 450

#include "frame.glsl"
#include "object.glsl"

// Vertex attributes, fed from the vertex buffer. The locations match
// Vertex::attributes() in engine/render/src/mesh.cpp - nothing validates that
// pairing, so it has to be kept in step by hand.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vWorldPosition;

void main() {
    vec4 worldPosition = object.model * vec4(inPosition, 1.0);

    // Right to left: model space -> world space -> view space -> clip space.
    gl_Position = frame.projection * frame.view * worldPosition;

    // Normals do NOT transform like positions. Scale a box twice as wide and
    // its diagonal faces tilt; the surface turns one way and a direction
    // multiplied by the same matrix turns the other. The inverse transpose is
    // the matrix that fixes that, and it reduces to the plain rotation when the
    // scale is uniform - so this is only doing real work when it has to.
    vNormal = transpose(inverse(mat3(object.model))) * inNormal;

    vUV = inUV;
    vWorldPosition = worldPosition.xyz;
}
