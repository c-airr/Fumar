#!/bin/sh
# Fetches the stock material library into assets/materials.
#
# The library - ten 2K texture sets from ambientCG, CC0 - lives on its own
# branch, `materials`, so that main stays small to check out. This takes the
# assets/materials folder from that branch and unpacks it into the working tree
# without checking the branch out or touching the index: assets/ is git-ignored
# on main, so nothing shows up as a change.
#
# Usage:
#     tools/get-materials.sh
#
# Run it again to update. Then build, or just start the editor: a development
# build reads the library from assets/materials in the source tree.

set -eu

repo=$(cd "$(dirname "$0")/.." && pwd)

if ! git -C "$repo" fetch origin materials; then
    echo "get-materials: could not fetch the 'materials' branch from origin" >&2
    exit 1
fi

if ! git -C "$repo" archive --format=tar origin/materials assets/materials | tar -x -C "$repo"; then
    echo "get-materials: could not unpack assets/materials from the 'materials' branch" >&2
    exit 1
fi

count=$(find "$repo/assets/materials" -name '*.fmat' | wc -l | tr -d ' ')
echo "get-materials: $count material(s) in assets/materials"
