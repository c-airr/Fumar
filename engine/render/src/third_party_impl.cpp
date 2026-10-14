// Single-header libraries put their definitions behind an implementation macro
// so the header can be included freely elsewhere without duplicate symbols.
// This translation unit is where those definitions get compiled, and it
// contains nothing else - so editing our own code never recompiles them.

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

// Writing PNGs: material maps pulled out of a glTF to stand beside a .fmat.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
