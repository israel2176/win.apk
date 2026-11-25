#!/usr/bin/env bash
# build.sh <work dir> [log tag]: the graphics DLL, aosp-hwui.dll -- AOSP 14's HWUI with its render
# thread and GL pipeline, Skia with Ganesh GL, minikin, HarfBuzz, FreeType, ICU and the rest --
# built from source for Windows x64 with clang, as Soong builds them for a device (windows/README.md,
# "The graphics DLL").
#
#   <work dir>/src   fetch.sh's sources (fetch.sh <work dir> first)
#   <work dir>/b     the build; aosp-hwui.dll and icudt72l.dat (ICU's data) land here, which
#                    build_aosp_gfx.sh stages (WINAPK_HWUI_DIR; CI's is thirdparty/aosp-hwui/b)
#
# On Linux it cross-compiles with clang for MinGW-w64 (windows/cmake/clang-mingw-x86_64.cmake);
# under MSYS2's CLANG64 (CI) it builds natively. Each run applies patch_src.py, regenerates the
# CMake modules from Android.bp (gen.py, mods.sh), builds, and summarizes any compile errors.
# HWUI_CMAKE_ARGS adds CMake arguments (CI: ccache as the compiler launcher).
set -u
H=$(cd "$(dirname "$0")" && pwd)
V=$1
TAG=${2:-build}
WINAPK=$(cd "$H/../.." && pwd)
. "$H/mods.sh"
PY=$(command -v python3 || command -v python)
"$PY" "$H/patch_src.py" "$V/src" || exit 1
"$PY" "$H/gen.py" "$V/src" "$H/cmake/modules.cmake" "$MODS" "$ANDROID" "$DROP" || exit 1
mkdir -p "$V/b"
cd "$V/b" || exit 1
case "$(uname -s)" in
    MINGW* | MSYS*) TOOLCHAIN=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++) ;;
    *) TOOLCHAIN=(-DCMAKE_TOOLCHAIN_FILE="$WINAPK/windows/cmake/clang-mingw-x86_64.cmake") ;;
esac
# shellcheck disable=SC2086
cmake -G Ninja "${TOOLCHAIN[@]}" -DCMAKE_BUILD_TYPE=Release -DAOSP="$V/src" -DWINAPK="$WINAPK" \
    ${HWUI_CMAKE_ARGS:-} "$H/cmake" > cfg.log 2>&1 || { grep -A5 "Error" cfg.log | head -20; exit 1; }
ninja -k 0 ${TARGETS:-} > "$TAG.log" 2>&1
status=$?
echo "failed: $(grep -c '^FAILED' "$TAG.log")"
grep -A14 "^FAILED" "$TAG.log" | grep "error:" | sed 's/.*error: //' | sort | uniq -c | sort -rn | head -${TOP:-30}
cp -f "$V/src/icu4c/source/stubdata/icudt72l.dat" "$V/b/icudt72l.dat"
exit $status
