# ---------------------------------------------------------------------------
#  fumar_native - C++ gameplay, loaded and reloaded at runtime.
#
#  The counterpart to fumar_script. It holds the interface a game library
#  implements (component.hpp) and the machinery that loads one, which is why
#  the interface header is PUBLIC: the game library compiles against exactly
#  the same declarations the engine does.
# ---------------------------------------------------------------------------

add_library(fumar_native STATIC
    src/dynamic_library.cpp
    src/native_engine.cpp)

add_library(fumar::native ALIAS fumar_native)

target_include_directories(fumar_native PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(fumar_native
    PUBLIC
        fumar::core
        fumar::scene
        # The script API's HEADERS, not the Lua engine behind them. Components
        # get the same input, camera and raycast a Lua script does, because the
        # two are meant to be one API from two languages - but a C++ component
        # has no reason to link an interpreter, and linking it would put
        # LuaJIT's ExternalProject in the game library's dependency graph,
        # where its build step would re-run on every press of Compile.
        fumar::script_api)

if(UNIX)
    # dlopen and friends. A separate library on older glibc, folded into libc
    # on newer ones, where naming it is harmless.
    target_link_libraries(fumar_native PRIVATE ${CMAKE_DL_LIBS})
endif()

# Where the editor's Compile button sends its work.
#
# Baked at configure time because it can only be known then, and absent from a
# distributed build - which is correct: there is no compiler on a player's
# machine and the button says so rather than failing strangely.
#
# Paths only, never a whole command line. Quoting a command inside a CMake
# string that becomes a C++ string literal means escaping for two parsers at
# once, and the compiler's complaint when you get it wrong ("invalid suffix on
# literal") points nowhere near the cause. The command is assembled in C++,
# where there is one set of quoting rules.
target_compile_definitions(fumar_native PRIVATE
    FUMAR_GAME_BUILD_DIR="${CMAKE_BINARY_DIR}"
    FUMAR_GAME_SOURCE_DIR="${CMAKE_SOURCE_DIR}/game/src"

    # The repository root, which is what an IDE should be handed rather than a
    # lone .cpp: both Visual Studio and VS Code read CMakePresets.json from here
    # and configure a real project from it. Given one file instead, they open a
    # text window with no build system behind it and no completion.
    FUMAR_PROJECT_DIR="${CMAKE_SOURCE_DIR}")

if(WIN32)
    # Through dev.ps1, not straight to cmake. Building with Ninja and clang-cl
    # needs the MSVC environment (INCLUDE/LIB), which the editor process does
    # not have unless it happened to be launched from a developer shell.
    target_compile_definitions(fumar_native PRIVATE
        FUMAR_GAME_DEV_SCRIPT="${CMAKE_SOURCE_DIR}/scripts/dev.ps1")
endif()

fumar_configure_target(fumar_native)
