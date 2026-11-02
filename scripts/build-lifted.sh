#!/bin/sh
# Builds the lifted C++ that hexlith-lift --emit wrote into the workspace.
#
# Usage: scripts/build-lifted.sh LIFT_DIR [CMAKE_ARGS...]
#
# The sources are derived from the game and live in the workspace, so they are
# built from there, into LIFT_DIR/build. ccache is used when it is installed,
# since a change to the lifter regenerates a shard only where its output
# changed.

set -eu

if [ "$#" -lt 1 ]; then
    echo "usage: $0 LIFT_DIR [CMAKE_ARGS...]" >&2
    exit 2
fi

lift_dir=$1
shift
source_dir=$(cd "$(dirname "$0")/.." && pwd)

launcher=""
if command -v ccache > /dev/null 2>&1; then
    launcher="-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
fi

cmake -S "$lift_dir" -B "$lift_dir/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DHEXLITH_SOURCE_DIR="$source_dir" \
    $launcher "$@"
cmake --build "$lift_dir/build"
