#!/usr/bin/env bash
#
# Builds fumar on Linux, installing what a bare container is missing first.
#
# Written to be run either on a developer machine (where apt-get is skipped
# because the packages are already there) or inside a throwaway container, which
# is how `offload run` uses it. Anything already installed costs nothing.

set -euo pipefail

PRESET="${1:-linux-debug}"

# --- dependencies -----------------------------------------------------------
# Only attempted as root: on a normal machine the packages are already present
# and asking for a password from a build script would be worse than failing.
if [ "$(id -u)" = "0" ] && command -v apt-get >/dev/null 2>&1; then
    echo "--- installing build dependencies ---"
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq

    # clang and ninja build fumar; make and gcc build LuaJIT, whose own makefile
    # expects them. libvulkan-dev provides the headers and the loader, glslc
    # compiles the shaders. The X11 and Wayland packages are what SDL needs to
    # produce a window at all - without them it configures itself into a build
    # with no video backend.
    apt-get install -y -qq --no-install-recommends \
        clang cmake ninja-build make gcc git ca-certificates pkg-config \
        libvulkan-dev glslc \
        libx11-dev libxext-dev libxrandr-dev libxi-dev libxcursor-dev \
        libxfixes-dev libxss-dev libxtst-dev \
        libwayland-dev wayland-protocols libxkbcommon-dev \
        libasound2-dev
fi

echo "--- versions ---"
clang --version | head -1
cmake --version | head -1
ninja --version
glslc --version | head -1

echo "--- configure ---"
cmake --preset "$PRESET"

echo "--- build ---"
cmake --build --preset "$PRESET"

echo "--- artifacts ---"
ls -la "build/${PRESET}/bin/"
