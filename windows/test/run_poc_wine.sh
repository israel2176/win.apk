#!/usr/bin/env bash
# run_poc_wine.sh -- build the loader, linker and arm64 tests on Linux and run
# them under Wine.
#
# Needs: clang, lld, cmake, ninja, mingw-w64 (headers/CRT), zlib for mingw-w64, wine64; for the
# arm64 test's translations a clang 19 or later (clang-19, -20 or -21, or WINAPK_AOT_CLANG).
#   Ubuntu: apt install clang lld cmake ninja-build mingw-w64 libz-mingw-w64-dev wine64
#
# Wine is a convenience, not the verdict: the CI job on a real Windows runner
# (.github/workflows/ci.yml) is what counts.

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
WIN_DIR="$(cd -- "$HERE/.." >/dev/null 2>&1 && pwd)"
BUILD="${WINAPK_BUILD:-$WIN_DIR/build}"

"$HERE/build_guest.sh" "$BUILD/guest"

cmake -S "$WIN_DIR" -B "$BUILD/host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$WIN_DIR/cmake/clang-mingw-x86_64.cmake" >/dev/null
cmake --build "$BUILD/host"

WINE="${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)}"
WINEDEBUG=-all "$WINE" "$BUILD/host/winapk-elfrun.exe" \
    "$BUILD/guest/libwinapk_poc_guest.so" \
    "$BUILD/guest/libwinapk_poc_guest_packed.so"
WINEDEBUG=-all "$WINE" "$BUILD/host/winapk-linker-test.exe" "$BUILD/guest/link"
# Translate the arm64 test libraries ahead of time, as CI's step does on Windows; without the
# translations the test's AOT engine has nothing to load and three of its checks fail. The flags
# are the background translation's (kAotCompileFlags in host/runtime_main.cpp) but for
# -march=native: with FMA the compiler inlines fma(), while everything else here calls
# mingw-w64's, which Ubuntu's msvcrt build rounds twice, so the engines would disagree in the
# last bit (on Windows the UCRT's is exact and they agree). x86-64-v2 has the SSE4.1 rounding the
# generated C needs. That C uses __builtin_elementwise_popcount: clang 19 or later.
AOT_CLANG="${WINAPK_AOT_CLANG:-$(command -v clang-21 || command -v clang-20 || command -v clang-19 || command -v clang)}"
rm -f "$BUILD"/guest/arm64/*.aot.stamp  # what an earlier run learned (a library that patches itself)
for so in "$BUILD"/guest/arm64/*.so; do
    WINEDEBUG=-all "$WINE" "$BUILD/host/winapk-aot.exe" "$so" "$so.aot.c" >/dev/null
    "$AOT_CLANG" --target=x86_64-w64-mingw32 -fuse-ld=lld -O2 -mllvm -fast-isel -march=x86-64-v2 \
        -fno-strict-aliasing -w -shared -o "$so.aot.dll" "$so.aot.c"
done
WINEDEBUG=-all "$WINE" "$BUILD/host/winapk-arm64-test.exe" "$BUILD/guest/arm64"
SCRATCH="$BUILD/scratch"
mkdir -p "$SCRATCH"
WINEDEBUG=-all "$WINE" "$BUILD/host/winapk-crash-test.exe" "$(WINEDEBUG=-all "$WINE" winepath -w "$SCRATCH")"
