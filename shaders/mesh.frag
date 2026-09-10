#version 450

// Surface shading without ray tracing: no shadows, no ambient occlusion.
//
// Built for every GPU. The shading model is identical to the ray traced variant
// beside it - the two differ only in what raytrace.glsl compiles to, which
// without FUMAR_RAY_QUERY is a pair of functions that return 1.0.

#include "mesh_shading.glsl"
