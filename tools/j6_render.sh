#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build tools/j6_render.c with every engine source and render the J-6 demo:
#   tools/j6_render.sh [out.wav]        default build/j6/demo.wav
# CC and J6_CFLAGS may be set (tests/j6/run_tests.sh passes its own).
set -eu
cd "$(dirname "$0")/.." || exit 1
CC=${CC:-cc}
SRC=firmware/src/j6
CFLAGS=${J6_CFLAGS:-"-std=c99 -O2 -g -Wall -Wextra -Werror -Wdouble-promotion -Wfloat-equal -Wvla -I$SRC"}
mkdir -p build/j6
# shellcheck disable=SC2086
$CC $CFLAGS -I"$SRC" -o build/j6/j6_render tools/j6_render.c "$SRC"/*.c
build/j6/j6_render "${1:-build/j6/demo.wav}"
