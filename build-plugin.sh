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

# MSYS_NO_PATHCONV stops the shell from rewriting /e/... into E:\... for the
# native gcc, so the paths are converted explicitly instead. Without this
# gcc receives a literal "/e/libmochiberry/..." and reports "No such file".
win_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -w "$1"
    else
        printf '%s' "$1"
    fi
}

SRC_W="$(win_path "$SRC_DIR")"
ENGINE_W="$(win_path "$ENGINE_DIR")"

echo "== $OUT =="
"$CC" -shared -O2 -o "$(win_path "$OUT")" \
    "$(win_path "$SRC_DIR/mochinode/mochinode_dynalib.c")" \
    "$(win_path "$SRC_DIR/libdynalib/dynalib.c")" \
    "$(win_path "$SRC_DIR/libdynalib/dynalib_abi.c")" \
    "$(win_path "$SRC_DIR/libdynalib/dynalib_trampoline.c")" \
    "-I$ENGINE_W" "-I$(win_path "$SRC_DIR/libdynalib")"

echo "== sqlite + net bridges =="
"$CC" -shared -O2 -o mochinode_sqlite.dll "$(win_path "$SRC_DIR/mochinode/mochinode_sqlite.c")" "-I$ENGINE_W"
"$CC" -shared -O2 -o mochinode_net.dll    "$(win_path "$SRC_DIR/mochinode/mochinode_net.c")"    "-I$ENGINE_W" -lws2_32

echo "done: $OUT mochinode_sqlite.dll mochinode_net.dll"
