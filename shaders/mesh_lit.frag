#version 450

// The colour arriving here was computed per vertex on the CPU. Writing it out
// is the whole fragment shader: no material, no lights, no textures.

layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(vColor, 1.0);
}
