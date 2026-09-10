// Per-draw data, written straight into the command buffer.
//
// A push constant range belongs to the PIPELINE, not to a stage: the vertex and
// fragment shaders of one pipeline share a single block and must declare it
// identically. The same goes for the C++ side - ObjectPushConstants in
// engine/render/src/renderer.cpp is this block, and nothing checks that they
// agree, so it lives in one file here to keep the number of places that can
// drift as low as possible.
//
// The guaranteed minimum size is 128 bytes. This block is 92, which is why the
// material parameters fit here rather than needing a buffer.

#ifndef FUMAR_OBJECT_GLSL
#define FUMAR_OBJECT_GLSL

layout(push_constant) uniform PushConstants {
    mat4 model;      // 64 bytes
    vec4 baseColor;  // 16

    /// 0 = dielectric (plastic, stone, wood), 1 = metal. Values in between are
    /// not a physical material, they exist because a texture that blends
    /// between the two has to interpolate somewhere.
    float metallic;  // 4

    /// 0 = mirror, 1 = completely diffuse. This is the single control that most
    /// changes what a surface looks like.
    float roughness; // 4

    /// 0 = normal, 0.5 = under the cursor, 1 = selected.
    float highlight; // 4
} object;

#endif // FUMAR_OBJECT_GLSL
