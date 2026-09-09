# ============================================================================
#  GLSL -> SPIR-V shader compilation.
#
#  Vulkan does not accept shader source text, it wants SPIR-V bytecode. glslc
#  (shipped with the Vulkan SDK) translates GLSL into SPIR-V, and we wire it in
#  as a build step so editing a shader triggers a rebuild just like editing a
#  .cpp file does.
# ============================================================================

if(NOT Vulkan_GLSLC_EXECUTABLE)
    message(FATAL_ERROR
        "fumar: glslc not found. Install the Vulkan SDK and make sure the "
        "VULKAN_SDK environment variable points at it.")
endif()

# ----------------------------------------------------------------------------
#  fumar_add_shaders(<target>
#      [OUTPUT_DIR <dir>]         # defaults to <exe dir>/shaders
#      FILES <file.vert> <file.frag> ...)
#
#  Each file is compiled to <OUTPUT_DIR>/<name>.spv (e.g. triangle.vert.spv).
# ----------------------------------------------------------------------------
function(fumar_add_shaders TARGET)
    cmake_parse_arguments(ARG "" "OUTPUT_DIR" "FILES" ${ARGN})

    if(NOT ARG_FILES)
        message(FATAL_ERROR "fumar_add_shaders(${TARGET}): FILES list is empty")
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
                    --target-env=vulkan1.3
                    $<$<CONFIG:Debug>:-g>
                    -MD -MF "${spv}.d"
                    -o "${spv}"
                    "${shader_abs}"
            DEPENDS "${shader_abs}"
            DEPFILE "${spv}.d"
            COMMENT "glslc ${shader_name}"
            VERBATIM)

        list(APPEND spv_files "${spv}")
    endforeach()

    # A separate target for the shaders plus a dependency edge, so ninja builds
    # them before the executable.
    add_custom_target(${TARGET}_shaders DEPENDS ${spv_files})
    add_dependencies(${TARGET} ${TARGET}_shaders)

    # Surface the shader sources in the IDE without trying to compile them as C++.
    target_sources(${TARGET} PRIVATE ${ARG_FILES})
    set_source_files_properties(${ARG_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)
endfunction()
