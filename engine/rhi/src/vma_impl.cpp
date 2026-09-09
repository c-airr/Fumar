// VulkanMemoryAllocator is a single-header library: the declarations are always
// visible, but the definitions only exist in the one translation unit that
// defines VMA_IMPLEMENTATION. This file is that translation unit, and it does
// nothing else - putting the macro in a file that also has our own code would
// mean recompiling all ~20k lines of VMA on every edit.

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
