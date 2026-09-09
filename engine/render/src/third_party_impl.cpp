// Single-header libraries put their definitions behind an implementation macro
// so the header can be included freely elsewhere without duplicate symbols.
// This translation unit is where those definitions get compiled, and it
// contains nothing else - so editing our own code never recompiles them.

#define STB_IMAGE_IMPLEMENTATION
// We only decode; the writer and resizer are dead weight otherwise.
#define STBI_NO_STDIO_WRITE
#include <stb_image.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
