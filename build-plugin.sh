#!/usr/bin/env bash
#
# build-plugin.sh — build the MochiBerry engine plugin bridge.
#
# Usage:
#   ./build-plugin.sh <engine-source-dir> [out-name]
#
# The engine dir must contain mbjs.h (e.g. E:/bcf/mochiberry).
# The resulting DLL must be placed where the engine's module loader can
# find it (SEVM: mochi_modules/), and the engine build must export the
# MBJS_* symbols (build_manual does; the stripped tier-core does not).
set -eu

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENGINE_DIR="${1:?usage: ./build-plugin.sh <engine-source-dir>}"
OUT="${2:-mochinode_dynalib.dll}"

CC="${CC:-gcc}"
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

echo "== $OUT =="
"$CC" -shared -O2 -o "$OUT" \
    "$SRC_DIR/mochinode/mochinode_dynalib.c" \
    "$SRC_DIR/libdynalib/dynalib.c" \
    "$SRC_DIR/libdynalib/dynalib_abi.c" \
    "$SRC_DIR/libdynalib/dynalib_trampoline.c" \
    -I"$ENGINE_DIR" -I"$SRC_DIR/libdynalib"

echo "== sqlite + net bridges =="
"$CC" -shared -O2 -o mochinode_sqlite.dll "$SRC_DIR/mochinode/mochinode_sqlite.c" -I"$ENGINE_DIR"
"$CC" -shared -O2 -o mochinode_net.dll    "$SRC_DIR/mochinode/mochinode_net.c"    -I"$ENGINE_DIR" -lws2_32

echo "done: $OUT mochinode_sqlite.dll mochinode_net.dll"
