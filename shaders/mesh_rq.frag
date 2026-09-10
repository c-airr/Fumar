#version 460

// Surface shading with hardware ray tracing.
//
// GL_EXT_ray_query lets an ORDINARY fragment shader trace a ray, rather than
// replacing the pipeline with ray generation, miss and hit shaders and a shader
// binding table to dispatch between them. That makes it a small addition to the
// shading already happening here instead of a second renderer, which is why it
// is where fumar starts.
//
// Requires GLSL 460: ray query is not available in earlier versions.
#extension GL_EXT_ray_query : require

// Reflections need to read the geometry a ray landed on, and a ray does not
// know in advance which mesh that will be - so nothing can be bound for it in
// advance either. buffer_reference is the answer: the shader is handed the
// ADDRESS of a buffer and lays a struct over it, exactly as a C pointer works.
// int64 is what an address is; scalar_block_layout is what lets the struct
// match the packing C++ used, instead of std430 padding every member to
// sixteen bytes.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#define FUMAR_RAY_QUERY 1

#include "mesh_shading.glsl"
