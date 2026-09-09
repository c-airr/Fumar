# ============================================================================
#  fumar - a 3D graphics engine built on Vulkan
#  Top level file: sets the language standard, pulls dependencies, adds modules.
# ============================================================================

cmake_minimum_required(VERSION 3.25)

# An in-source build litters the repository with intermediate files and mixes
# them with the code.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not allowed.\n"
        "Use a preset instead, e.g.: cmake --preset windows-debug")
endif()

project(fumar
    VERSION 0.1.0
    DESCRIPTION "A 3D graphics engine built on Vulkan"
    LANGUAGES C CXX)

# --- language standard ------------------------------------------------------
# C is enabled because VMA and some dependencies are C libraries.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --- project options --------------------------------------------------------
option(FUMAR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# --- C runtime --------------------------------------------------------------
# Link the C and C++ runtimes statically on Windows.
#
# The default is the DLL runtime, which means the executable needs the Visual
# C++ redistributable installed - and in a debug build it needs the DEBUG
# redistributable, which only ships with Visual Studio and cannot legally be
# distributed at all. Static linking makes the binary self-contained at the
# cost of a few hundred kilobytes.
#
# This has to be set before any target is created, and every target in the
# build (including SDL) must agree - mixing static and dynamic runtimes in one
# process gives two separate heaps and crashes when memory allocated by one is
# freed by the other.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- artifact layout --------------------------------------------------------
# All executables land in one directory, with the shaders/ folder holding the
# compiled .spv files right next to them.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Fallback in case someone configures without a preset.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# --- Vulkan -----------------------------------------------------------------
# We only take the headers and glslc from the SDK. The vulkan-1 library is
# deliberately NOT linked: function pointers are resolved at runtime through
# vkGetInstanceProcAddr (see engine/rhi/src/instance.cpp), so the engine works
# with whatever loader the system provides.
find_package(Vulkan REQUIRED)
message(STATUS "fumar: Vulkan headers -> ${Vulkan_INCLUDE_DIRS}")
message(STATUS "fumar: shader compiler -> ${Vulkan_GLSLC_EXECUTABLE}")

include(FumarWarnings)
include(FumarShaders)
include(FumarVulkan)
include(FumarDependencies)

# --- engine modules ---------------------------------------------------------
# The order mirrors the dependency graph: core depends on nothing, sandbox
# depends on everything.
add_subdirectory(engine/core)
add_subdirectory(engine/platform)
add_subdirectory(engine/rhi)
add_subdirectory(engine/render)
add_subdirectory(sandbox)

# --- distribution -----------------------------------------------------------
# `cmake --install build/<preset> --prefix dist` produces a folder that can be
# copied to another machine and run: the executable, the compiled shaders and
# whatever assets exist, and nothing else.
install(TARGETS sandbox RUNTIME DESTINATION .)
# FILES_MATCHING keeps the .d dependency files glslc leaves behind out of the
# distribution - they are a build artifact, useless at runtime.
install(DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/" DESTINATION shaders
        FILES_MATCHING PATTERN "*.spv")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/assets/" DESTINATION assets OPTIONAL)

message(STATUS "fumar ${PROJECT_VERSION} | ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} | ${CMAKE_BUILD_TYPE}")
