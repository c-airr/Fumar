#version 450

#include "frame.glsl"
#include "object.glsl"

// The GPU-workload path. A compute shader has already multiplied the node
// hierarchy into `worlds`; this stage only applies that matrix to the vertex.
// Balanced does the same multiply from a push constant that the CPU wrote.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vWorldPosition;
layout(location = 3) out vec4 vTangent;

layout(std430, set = 1, binding = 0) readonly buffer WorldMatrices {
    mat4 worlds[];
};

void main() {
    const mat4 model = worlds[object.nodeIndex];
    const vec4 worldPosition = model * vec4(inPosition, 1.0);

    gl_Position = frame.projection * frame.view * worldPosition;
    vNormal = transpose(inverse(mat3(model))) * inNormal;
    vUV = inUV;
    vWorldPosition = worldPosition.xyz;
    vTangent = vec4(mat3(model) * inTangent.xyz, inTangent.w);
}
