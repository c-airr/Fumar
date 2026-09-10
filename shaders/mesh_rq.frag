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

#define FUMAR_RAY_QUERY 1

#include "mesh_shading.glsl"
