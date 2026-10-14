// Per-draw data, written straight into the command buffer.
//
// A push constant range belongs to the PIPELINE, not to a stage: the vertex and
// fragment shaders of one pipeline share a single block and must declare it
// identically. The same goes for the C++ side - ObjectPushConstants in
// engine/render/src/renderer.cpp is this block, and nothing checks that they
// agree, so it lives in one file here to keep the number of places that can
// drift as low as possible.
//
// The guaranteed minimum size is 128 bytes; this block is 88. What a surface
// looks like is not here: it is a record in the material buffer (see
// material.glsl), and a draw names it by index.

#ifndef FUMAR_OBJECT_GLSL
#define FUMAR_OBJECT_GLSL

layout(push_constant) uniform PushConstants {
    mat4 model;          // 64 bytes, offset 0

    /// The flat colour the outline pass draws light markers in. The mesh
    /// pipeline ignores it.
    vec4 overlayColor;   // 16,       offset 64

    /// Which record of the material buffer this draw wears.
    uint materialIndex;  // 4,        offset 80

    /// 0 = normal, 0.5 = under the cursor, 1 = selected, < 0 = draw the
    /// overlay colour instead.
    float highlight;     // 4,        offset 84
} object;

#endif // FUMAR_OBJECT_GLSL
