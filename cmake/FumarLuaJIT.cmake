# ============================================================================
#  LuaJIT.
#
#  LuaJIT ships no CMake build - it has a hand-written makefile on Unix and a
#  batch script on Windows, both of which do things CMake cannot express: they
#  build a host tool (buildvm) first, run it to generate assembly and headers,
#  and only then compile the library. Rewriting that in CMake is a maintenance
#  burden for no gain, so ExternalProject drives the upstream build instead.
#
#  The consequence is that LuaJIT is built during the BUILD step, not during
#  configure - hence BUILD_BYPRODUCTS and an IMPORTED target, which is how a
#  library that does not exist yet at configure time is described.
# ============================================================================

include(ExternalProject)

set(FUMAR_LUAJIT_ROOT "${CMAKE_BINARY_DIR}/_deps/luajit-src")
set(FUMAR_LUAJIT_SRC "${FUMAR_LUAJIT_ROOT}/src")

if(WIN32)
    # The C runtime has to match the rest of the build exactly. Mixing a release
    # CRT into a debug process gives two heaps, and memory allocated on one side
    # and freed on the other corrupts both.
    #
    # msvcbuild.bat has no flag for this, but cl.exe prepends whatever is in the
    # CL environment variable to its command line - the documented way to inject
    # flags into a build you do not control.
    if(CMAKE_BUILD_TYPE MATCHES "[Dd]ebug")
        set(_luajit_crt "/MTd")
    else()
        set(_luajit_crt "/MT")
    endif()

    set(_luajit_library "${FUMAR_LUAJIT_SRC}/lua51.lib")

    # Driven through cmake -E rather than a shell or a generated batch file.
    #
    # `-E env` sets a variable for the child, `-E chdir` runs it in a directory,
    # and both take their arguments as a list - so nothing has to be quoted,
    # escaped, or survive a round trip through cmd.exe parsing. A hand-written
    # .bat needs all three of those to line up and fails silently when they do
    # not.
    # --unset=NoDefaultCurrentDirectoryInExePath is the load-bearing part here.
    #
    # When that variable is set - and some terminals and tools set it - cmd.exe
    # stops looking in the working directory for programs to run. LuaJIT builds
    # host tools (minilua, buildvm) into its own source directory and then calls
    # them by bare name, so under that setting its build fails partway through
    # with "'minilua' is not recognized", long after the script itself started
    # fine. Unsetting it for this one command restores the assumption the
    # upstream build makes.
    #
    # The script is still named by full path, because the same rule applies to
    # finding it in the first place. chdir is needed regardless: msvcbuild.bat
    # refers to its sources relatively.
    set(_luajit_build_command
        "${CMAKE_COMMAND}" -E env "CL=${_luajit_crt}"
        --unset=NoDefaultCurrentDirectoryInExePath
        "${CMAKE_COMMAND}" -E chdir "${FUMAR_LUAJIT_SRC}"
        cmd /c "${FUMAR_LUAJIT_SRC}/msvcbuild.bat" static)
else()
    set(_luajit_library "${FUMAR_LUAJIT_SRC}/libluajit.a")
    set(_luajit_build_command make -C "${FUMAR_LUAJIT_ROOT}" BUILDMODE=static)
endif()

ExternalProject_Add(luajit_external
    GIT_REPOSITORY https://github.com/LuaJIT/LuaJIT.git
    # LuaJIT publishes no release tags; v2.1 is a rolling branch, so a commit is
    # the only reproducible pin.
    GIT_TAG        c6ffc141a8762b41703f9287d63d93622a13dd8f
    GIT_SHALLOW    FALSE
    SOURCE_DIR     "${FUMAR_LUAJIT_ROOT}"
    # Its build system writes next to the sources and cannot be told otherwise.
    BUILD_IN_SOURCE TRUE
    CONFIGURE_COMMAND ""
    BUILD_COMMAND  ${_luajit_build_command}
    INSTALL_COMMAND ""
    # Tells ninja this command produces that file, so other targets can depend
    # on it even though it does not exist until the build runs.
    BUILD_BYPRODUCTS "${_luajit_library}"
    LOG_BUILD      TRUE
    LOG_OUTPUT_ON_FAILURE TRUE)

# The include directory has to exist at configure time or CMake refuses to put
# it on an interface, and ExternalProject only creates it during the build.
file(MAKE_DIRECTORY "${FUMAR_LUAJIT_SRC}")

add_library(fumar_luajit STATIC IMPORTED GLOBAL)
set_target_properties(fumar_luajit PROPERTIES
    IMPORTED_LOCATION "${_luajit_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${FUMAR_LUAJIT_SRC}")

# An IMPORTED target cannot carry a dependency of its own, so anything linking
# it has to be ordered after the external build. fumar_script does that.
add_dependencies(fumar_luajit luajit_external)

message(STATUS "fumar: LuaJIT -> ${_luajit_library}")
