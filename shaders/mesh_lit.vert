#version 450

#include "frame.glsl"

// Vertices the CPU already placed in world space and already lit. This stage
// only projects them: the transform and the lighting happened before upload.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 vColor;

void main() {
    gl_Position = frame.projection * frame.view * vec4(inPosition, 1.0);
    vColor = inColor;
}
