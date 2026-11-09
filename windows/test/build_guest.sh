#!/usr/bin/env bash
# build_guest.sh -- builds the PoC Android x86_64 (and arm64) libraries with
# plain clang + lld.
#
# No NDK needed: the guest declares its libc imports by hand and links with
# -nostdlib, so its undefined symbols are left for the Win.apk loader.
#
#   windows/test/build_guest.sh [out_dir]      (default: windows/build/guest)
#
# Two variants of the same source:
#   libwinapk_poc_guest.so         plain RELA relocations
#   libwinapk_poc_guest_packed.so  Android packed relocations (APS2 + RELR),
#                                  what NDK builds for recent minSdk produce
#
# And, in link/, the libraries for winapk-linker-test (see guest/link_guest.c).

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
OUT="${1:-$HERE/../build/guest}"
CC="${CC_ANDROID:-clang}"
mkdir -p "$OUT"

COMMON=(
    --target=x86_64-linux-android24
    -fPIC -O2 -fstack-protector-all -ffreestanding
    -nostdlib -shared -fuse-ld=lld
    -fvisibility=hidden
    -Wl,-z,relro -Wl,-z,now
)

"$CC" "${COMMON[@]}" -Wl,-soname,libwinapk_poc_guest.so \
    -o "$OUT/libwinapk_poc_guest.so" "$HERE/guest/poc_guest.c"

"$CC" "${COMMON[@]}" -Wl,-soname,libwinapk_poc_guest_packed.so \
    -Wl,--pack-dyn-relocs=android+relr -Wl,--use-android-relr-tags \
    -o "$OUT/libwinapk_poc_guest_packed.so" "$HERE/guest/poc_guest.c"

# The linker test's dependency tree. Stub system libraries give the DT_NEEDED
# entries an NDK build would have; they are only link inputs and never ship.
LINK="$OUT/link"
STUBS="$OUT/link-stubs"
mkdir -p "$LINK" "$STUBS"
for lib in libc libdl liblog libz; do
    "$CC" "${COMMON[@]}" -Wl,-soname,$lib.so -o "$STUBS/$lib.so" -x c /dev/null
done
# Real ELF TLS (the plugin has a __thread variable dlclose must free).
link_lib() {  # link_lib <part> <name> [linker args...]
    local part="$1" name="$2"
    shift 2
    "$CC" "${COMMON[@]}" -fno-emulated-tls -DWINAPK_LINK_$part -Wl,-soname,$name.so -Wl,--no-as-needed \
        -o "$LINK/$name.so" "$HERE/guest/link_guest.c" "$@"
}
# base and mid need each other: link base against a placeholder that only
# carries mid's soname, then link the real mid against base.
"$CC" "${COMMON[@]}" -Wl,-soname,libwinapk_link_mid.so -o "$STUBS/libwinapk_link_mid.so" -x c /dev/null
link_lib PLUGIN libwinapk_link_plugin
link_lib BASE libwinapk_link_base -L"$STUBS" -lwinapk_link_mid
link_lib MID libwinapk_link_mid -L"$LINK" -L"$STUBS" -lwinapk_link_base -llog
link_lib ROOT libwinapk_link_root -L"$LINK" -L"$STUBS" -lwinapk_link_mid -lc -ldl

# The Bionic shim's guest-side test (winapk-bionic-test). Real ELF TLS, as an
# NDK build for minSdk 29 or later uses.
"$CC" "${COMMON[@]}" -fno-emulated-tls -Wl,-soname,libwinapk_bionic_guest.so \
    -o "$OUT/libwinapk_bionic_guest.so" "$HERE/guest/bionic_guest.c" -L"$STUBS" -Wl,--no-as-needed -lz

# The arm64 engines' test libraries (winapk-arm64-test).
# arm64_isa.c three ways (the test compares each against the same code built
# for the host), and the bridge library against the Bionic shim. Atomics are
# inlined (-mno-outline-atomics): the outline helpers live in libgcc, which
# -nostdlib leaves out.
ARM64_OUT="$OUT/arm64"
mkdir -p "$ARM64_OUT"
ARM64=(
    --target=aarch64-linux-android24
    -fPIC -ffreestanding -nostdlib -shared -fuse-ld=lld
    -fvisibility=hidden -Wl,-z,relro -Wl,-z,now
)
"$CC" "${ARM64[@]}" -O2 -mno-outline-atomics -Wl,-soname,libwinapk_arm64_isa_O2.so \
    -o "$ARM64_OUT/libwinapk_arm64_isa_O2.so" "$HERE/guest/arm64_isa.c"
"$CC" "${ARM64[@]}" -O3 -mno-outline-atomics -Wl,-soname,libwinapk_arm64_isa_O3.so \
    -o "$ARM64_OUT/libwinapk_arm64_isa_O3.so" "$HERE/guest/arm64_isa.c"
"$CC" "${ARM64[@]}" -O3 -march=armv8.1-a -Wl,-soname,libwinapk_arm64_isa_lse.so \
    -o "$ARM64_OUT/libwinapk_arm64_isa_lse.so" "$HERE/guest/arm64_isa.c"
"$CC" "${ARM64[@]}" -O2 -fstack-protector-all -mno-outline-atomics \
    -Wl,--pack-dyn-relocs=android+relr -Wl,--use-android-relr-tags \
    -Wl,-soname,libwinapk_arm64_bridge.so \
    -o "$ARM64_OUT/libwinapk_arm64_bridge.so" "$HERE/guest/arm64_bridge_guest.c"
# ELF TLS through TLSDESC (arm64_tls_guest.c): API 29, real ELF TLS.
"$CC" "${ARM64[@]/aarch64-linux-android24/aarch64-linux-android29}" -O2 -fno-emulated-tls \
    -Wl,-soname,libwinapk_arm64_tls.so -o "$ARM64_OUT/libwinapk_arm64_tls.so" "$HERE/guest/arm64_tls_guest.c"
# Control flow and edge cases (arm64_flow_guest.c), and a library the test
# unloads with dlclose (arm64_isa.c again).
"$CC" "${ARM64[@]}" -O2 -mno-outline-atomics -Wl,-soname,libwinapk_arm64_flow.so \
    -o "$ARM64_OUT/libwinapk_arm64_flow.so" "$HERE/guest/arm64_flow_guest.c"
"$CC" "${ARM64[@]}" -O2 -mno-outline-atomics -Wl,-soname,libwinapk_arm64_unload.so \
    -o "$ARM64_OUT/libwinapk_arm64_unload.so" "$HERE/guest/arm64_isa.c"
# The same, linked 2 MB higher: the same code bytes at other addresses, which
# a translation of the first must not be taken for.
"$CC" "${ARM64[@]}" -O2 -mno-outline-atomics -Wl,-soname,libwinapk_arm64_rebased.so -Wl,--image-base=0x200000 \
    -o "$ARM64_OUT/libwinapk_arm64_rebased.so" "$HERE/guest/arm64_isa.c"
# A library that patches its own code (arm64_patch_guest.c), one copy per
# engine: the first patch retires a library's translation for good.
for engine in interp jit aot; do
    "$CC" "${ARM64[@]}" -O2 -Wl,-soname,libwinapk_arm64_patch_$engine.so \
        -o "$ARM64_OUT/libwinapk_arm64_patch_$engine.so" "$HERE/guest/arm64_patch_guest.c"
done
# C++ exceptions (arm64_eh_guest.cpp), with an NDK's libc++ and unwinder when
# there is one (GitHub's Ubuntu runners have it): the test skips it otherwise.
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_LATEST_HOME:-}}"
NDK_CXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++"
if [ -n "$NDK" ] && [ -x "$NDK_CXX" ]; then
    "$NDK_CXX" --target=aarch64-linux-android24 -O2 -fPIC -shared -static-libstdc++ -fvisibility=hidden \
        -Wl,-soname,libwinapk_arm64_eh.so -o "$ARM64_OUT/libwinapk_arm64_eh.so" "$HERE/guest/arm64_eh_guest.cpp"
else
    echo "[build_guest] no Android NDK (ANDROID_NDK_HOME): libwinapk_arm64_eh.so not built"
fi
# Random instruction sequences for the differential test (gen_arm64_fuzz.py:
# functions, instructions each, seed).
python3 "$HERE/guest/gen_arm64_fuzz.py" 400 48 1 > "$ARM64_OUT/arm64_fuzz.S"
"$CC" "${ARM64[@]}" -Wl,-soname,libwinapk_arm64_fuzz.so \
    -o "$ARM64_OUT/libwinapk_arm64_fuzz.so" "$ARM64_OUT/arm64_fuzz.S"
# The engines' benchmarks, -O2 like an app's release build.
"$CC" "${ARM64[@]}" -O2 -mno-outline-atomics -Wl,-soname,libwinapk_arm64_bench.so \
    -o "$ARM64_OUT/libwinapk_arm64_bench.so" "$HERE/guest/arm64_bench.c"

echo "[build_guest] $OUT/libwinapk_poc_guest.so"
echo "[build_guest] $OUT/libwinapk_poc_guest_packed.so"
echo "[build_guest] $LINK/libwinapk_link_{root,mid,base,plugin}.so"
echo "[build_guest] $OUT/libwinapk_bionic_guest.so"
echo "[build_guest] $ARM64_OUT/libwinapk_arm64_{isa_O2,isa_O3,isa_lse,bridge,flow,unload,rebased,patch_*,fuzz,bench}.so"
