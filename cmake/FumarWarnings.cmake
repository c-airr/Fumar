# ============================================================================
#  Shared compiler warning set.
#
#  This is an INTERFACE target: it produces no file, it only carries flags.
#  Every engine module links it PRIVATE, so the warnings apply to our code but
#  do NOT leak into the dependencies, nor into code that will one day consume
#  fumar as a library.
# ============================================================================

add_library(fumar_warnings INTERFACE)
add_library(fumar::warnings ALIAS fumar_warnings)

# The interesting warnings, in clang/gcc spelling. Shared by both branches
# below, because clang-cl understands these too - it is only the -Wall spelling
# that behaves differently.
set(_fumar_extra_warnings
    -Wshadow                # a local variable shadows another variable
    -Wnon-virtual-dtor      # polymorphic class without a virtual destructor
    -Wold-style-cast        # C-style cast instead of static_cast
    -Wcast-align            # cast that increases the required alignment
    -Woverloaded-virtual    # method hides a virtual method of the base
    -Wnull-dereference      # dereference of a possibly null pointer
    -Wdouble-promotion)     # silent float -> double promotion

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # clang-cl imitates cl.exe, where /Wall means literally everything - so it
    # maps -Wall onto clang's -Weverything. That pulls in -Wc++98-compat and
    # friends, which fire on every `using` alias in a C++20 codebase and bury
    # the warnings that matter. /W4 is the spelling that maps to the clang
    # -Wall -Wextra pair.
    target_compile_options(fumar_warnings INTERFACE /W4 ${_fumar_extra_warnings})
    if(FUMAR_WARNINGS_AS_ERRORS)
        target_compile_options(fumar_warnings INTERFACE /WX)
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(fumar_warnings INTERFACE
        -Wall
        -Wextra
        ${_fumar_extra_warnings})
    if(FUMAR_WARNINGS_AS_ERRORS)
        target_compile_options(fumar_warnings INTERFACE -Werror)
    endif()
elseif(MSVC)
    target_compile_options(fumar_warnings INTERFACE /W4 /permissive-)
    if(FUMAR_WARNINGS_AS_ERRORS)
        target_compile_options(fumar_warnings INTERFACE /WX)
    endif()
endif()

# ----------------------------------------------------------------------------
# Helper: a single place where every engine module gets the same settings.
# Usage:  fumar_configure_target(fumar_core)
# ----------------------------------------------------------------------------
function(fumar_configure_target TARGET)
    target_link_libraries(${TARGET} PRIVATE fumar::warnings)
    set_target_properties(${TARGET} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        # /EHsc - C++ exceptions on, extern "C" functions assumed not to throw.
        #         Required because vulkan.hpp reports errors through exceptions.
        # /utf-8 - treat sources and literals as UTF-8 regardless of the
        #          system locale.
        target_compile_options(${TARGET} PRIVATE /EHsc /utf-8)
    endif()
endfunction()
