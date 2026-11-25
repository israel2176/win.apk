#!/usr/bin/env bash
# build.sh <work dir> <log tag>: AOSP 14's HWUI and its closure for Windows x64 (clang, MinGW-w64).
# <work dir>/src holds fetch.sh's sources; the build goes to <work dir>/b. Regenerates the CMake
# modules from mods.sh, configures, builds, and summarizes the compile errors.
H=$(cd "$(dirname "$0")" && pwd)
V=$1
TAG=${2:-build}
WINAPK=$(cd "$H/../.." && pwd)
. "$H/mods.sh"
python3 "$H/patch_src.py" "$V/src" || exit 1
python3 "$H/gen.py" "$V/src" "$H/cmake/modules.cmake" "$MODS" "$ANDROID" "$DROP" || exit 1
mkdir -p "$V/b"
cd "$V/b" || exit 1
cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE="$WINAPK/windows/cmake/clang-mingw-x86_64.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DAOSP="$V/src" -DWINAPK="$WINAPK" "$H/cmake" > cfg.log 2>&1 || { grep -A5 "Error" cfg.log | head -20; exit 1; }
ninja -k 0 ${TARGETS:-} > "$TAG.log" 2>&1
echo "failed: $(grep -c '^FAILED' "$TAG.log")"
grep -A14 "^FAILED" "$TAG.log" | grep "error:" | sed 's/.*error: //' | sort | uniq -c | sort -rn | head -${TOP:-30}
