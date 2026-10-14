#version 450

// The Cpu workload's vertex shader: mesh.vert plus a second vertex stream.
//
// The positions arrive already in world space - the CPU transformed them - so
// the model matrix it is given is the identity, and everything below is
// mesh.vert unchanged. The extra stream is what the CPU traced for each
// vertex: how much of the sun, the sky and each lamp it can see, and the light
// bounced onto it. Passed straight through; the rasteriser blends it across
// each triangle.

#include "frame.glsl"
#include "object.glsl"

// Vertex attributes, fed from the vertex buffer. The locations match
// Vertex::attributes() in engine/render/src/mesh.cpp - nothing validates that
// pairing, so it has to be kept in step by hand.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vWorldPosition;

/// World-space tangent, w carried through as the handedness. A tangent lies IN
/// the surface, so it turns with the plain model matrix - unlike the normal,
/// which needs the inverse transpose.
layout(location = 3) out vec4 vTangent;

layout(location = 4) in vec4 inCpuVisibility;
layout(location = 5) in vec4 inCpuLamps;
layout(location = 6) in vec4 inCpuBounce;

layout(location = 4) out vec4 vCpuVisibility;
layout(location = 5) out vec4 vCpuLamps;
layout(location = 6) out vec3 vCpuBounce;

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
    vTangent = vec4(mat3(object.model) * inTangent.xyz, inTangent.w);

    vCpuVisibility = inCpuVisibility;
    vCpuLamps = inCpuLamps;
    vCpuBounce = inCpuBounce.rgb;
}
