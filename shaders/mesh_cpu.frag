#version 450

// Surface shading for the Cpu workload.
//
// The same shading model as the other two variants, evaluated per pixel on the
// GPU as always. What differs is where its visibility questions are answered:
// not by rays traced here, but by rays the CPU traced per vertex, which the
// vertex shader passes along and the rasteriser interpolates. See raytrace.glsl
// under FUMAR_CPU_LIGHTING.

#define FUMAR_CPU_LIGHTING 1

#include "mesh_shading.glsl"
