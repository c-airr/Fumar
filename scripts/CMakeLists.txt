# ---------------------------------------------------------------------------
#  scripts - gameplay, in both languages.
#
#      scripts/lua/            .lua, read by the engine at runtime
#      scripts/cpp/public/     .h   headers other files may include
#      scripts/cpp/private/    .cpp implementation, compiled into fumar_game
#
#  The public/private split is Unreal's convention and it earns its place for
#  the same reason there: public/ is what the rest of the module (and one day
#  another module) may include, private/ is everything that is nobody else's
#  business. With one pair of files it is a formality - the point is that the
#  build already treats the two directories differently, so it stays true as
#  this grows rather than having to be imposed later.
#
#  fumar_game is SHARED, and that is the whole feature rather than a packaging
#  detail: a shared library can be replaced while the process that loaded it is
#  still running. Compiled into the editor it would mean restarting for every
#  change, which is exactly what scripting exists to avoid.
# ---------------------------------------------------------------------------

# Globbed, against the usual advice, and deliberately.
#
# The normal objection is that CMake cannot know a new file appeared and will
# not reconfigure - which is exactly what CONFIGURE_DEPENDS fixes: Ninja
# re-checks the directory on every build and reruns CMake when it changed. The
# cost is one directory stat per build. The alternative is a person adding a
# .cpp in Visual Studio, pressing Compile, and being told nothing changed
# because a file they have never heard of needed editing first.
file(GLOB_RECURSE FUMAR_GAME_SOURCES CONFIGURE_DEPENDS "cpp/private/*.cpp")
file(GLOB_RECURSE FUMAR_GAME_HEADERS CONFIGURE_DEPENDS
    "cpp/public/*.h" "cpp/public/*.hpp"
    "cpp/private/*.h" "cpp/private/*.hpp")

if(NOT FUMAR_GAME_SOURCES)
    message(FATAL_ERROR
        "fumar: no .cpp files in scripts/cpp/private. The gameplay library needs "
        "at least one - a library with nothing in it cannot be linked.")
endif()

# The headers are listed as sources so they appear in IDE project trees. Nothing
# compiles them; a header in this list is a file you can find, not a build step.
add_library(fumar_game SHARED ${FUMAR_GAME_SOURCES} ${FUMAR_GAME_HEADERS})

target_include_directories(fumar_game
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/cpp/public"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/cpp/private")

target_link_libraries(fumar_game PRIVATE
    fumar::core
    fumar::scene
    fumar::native)

# Beside the executable, where NativeEngine looks for it. On Windows a DLL is a
# RUNTIME artifact and on Linux a .so is a LIBRARY one, so both have to be said.
set_target_properties(fumar_game PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"

    # Hidden by default, so the only symbol that escapes is the one marked for
    # export - which keeps the library's surface exactly as wide as the engine
    # needs it to be.
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# Mirrors the source layout, so the folder beside a released executable looks
# like the one in the repository. The .cpp does not travel - it is already
# inside fumar_game - but the .lua does, because the engine reads it at runtime.
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/cpp" FILES ${FUMAR_GAME_SOURCES}
             ${FUMAR_GAME_HEADERS})

fumar_configure_target(fumar_game)
