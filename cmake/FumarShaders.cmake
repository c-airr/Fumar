# ============================================================================
#  GLSL -> SPIR-V shader compilation.
#
#  Vulkan does not accept shader source text, it wants SPIR-V bytecode. glslc
#  (shipped with the Vulkan SDK) translates GLSL into SPIR-V, and we wire it in
#  as a build step so editing a shader triggers a rebuild just like editing a
#  .cpp file does.
# ============================================================================

# Flags are resolved once, at configure time, rather than with a generator
# expression per command. A genex like $<$<CONFIG:Debug>:-g> does NOT disappear
# in other configurations - it collapses to an empty argument, which glslc then
# treats as a second input file and refuses with "linking multiple files is not
# supported yet". Plain CMake variables have no such trap.
set(FUMAR_GLSLC_FLAGS --target-env=vulkan1.3 -c)
if(CMAKE_BUILD_TYPE MATCHES "[Dd]ebug")
    # -g keeps the GLSL source in the SPIR-V so RenderDoc and friends can show
    # it; -O0 stops the optimiser from reordering it out of recognition.
    list(APPEND FUMAR_GLSLC_FLAGS -g -O0)
else()
    list(APPEND FUMAR_GLSLC_FLAGS -O)
endif()

if(NOT Vulkan_GLSLC_EXECUTABLE)
    message(FATAL_ERROR
        "fumar: glslc not found. Install the Vulkan SDK and make sure the "
        "VULKAN_SDK environment variable points at it.")
endif()

# ----------------------------------------------------------------------------
#  fumar_compile_shaders(<target>
#      [OUTPUT_DIR <dir>]         # defaults to <exe dir>/shaders
#      FILES <file.vert> <file.frag> ...)
#
#  Creates a custom target that compiles the listed shaders to
#  <OUTPUT_DIR>/<name>.spv. Executables depend on it with
#  add_dependencies(); they must NOT each declare the shaders themselves,
#  because two targets producing the same output file is an error ninja
#  refuses outright ("multiple rules generate ...").
# ----------------------------------------------------------------------------
function(fumar_compile_shaders TARGET)
    cmake_parse_arguments(ARG "" "OUTPUT_DIR" "FILES" ${ARGN})

    if(NOT ARG_FILES)
        message(FATAL_ERROR "fumar_compile_shaders(${TARGET}): FILES list is empty")
    endif()

    set(out_dir "${ARG_OUTPUT_DIR}")
    if(NOT out_dir)
        set(out_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders")
    endif()

    set(spv_files "")
    foreach(shader IN LISTS ARG_FILES)
        get_filename_component(shader_abs "${shader}" ABSOLUTE)
        get_filename_component(shader_name "${shader}" NAME)
        set(spv "${out_dir}/${shader_name}.spv")

        # DEPFILE: glslc writes out the list of files pulled in via #include,
        # so touching a shared shader header also forces a recompile.
        add_custom_command(
            OUTPUT "${spv}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${out_dir}"
            COMMAND "${Vulkan_GLSLC_EXECUTABLE}"
                    ${FUMAR_GLSLC_FLAGS}
                    -MD -MF "${spv}.d"
                    -o "${spv}"
                    "${shader_abs}"
            DEPENDS "${shader_abs}"
            DEPFILE "${spv}.d"
            COMMENT "glslc ${shader_name}"
            VERBATIM)

        list(APPEND spv_files "${spv}")
    endforeach()

    # SOURCES surfaces the .vert/.frag files in IDE project trees without
    # anything trying to compile them as C++.
    add_custom_target(${TARGET} ALL DEPENDS ${spv_files} SOURCES ${ARG_FILES})
endfunction()
