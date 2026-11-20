#!/usr/bin/env bash
# Builds and runs the HEIF test (src/HeifTest.java): HEIF and AVIF pictures through AOSP 14's
# BitmapFactory and ImageDecoder on Robolectric's native runtime, their decoding natives redirected to
# windows/java/aosp_gfx's HostImageDecode as build_aosp_gfx.sh does (scripts/redirect_decoders.sh), with
# libheif's heif-convert standing in for the host's WIC (src/com/macapk/loader/NativeImageCodec.java).
# Three runs: a host decoder that turns the picture by its irot, one that leaves it as stored, and none
# (Windows without the HEIF extension).
#
#   windows/test/aospgfx/fetch.sh <deps>
#   windows/test/heif/run.sh <deps> <out>
#
# Linux only (the .so; the test's decoder runs heif-convert): needs a JDK (JAVA_HOME), a C compiler for
# test/aospgfx's redirect agent, and heif-convert with its HEVC and AV1 decoders (Ubuntu:
# libheif-examples libheif-plugin-libde265 libheif-plugin-dav1d). Not a CI job.
set -uo pipefail

DEPS=${1:?usage: run.sh <deps> <out>}
OUT=${2:?usage: run.sh <deps> <out>}
HERE=$(cd "$(dirname "$0")" && pwd)
: "${JAVA_HOME:?set JAVA_HOME to a JDK}"
mkdir -p "$OUT/classes"
OUT=$(cd "$OUT" && pwd)
DEPS=$(cd "$DEPS" && pwd)
ALL="$DEPS/android-all-14.jar"
NATIVE="$DEPS/nativeruntime/native/linux/x86_64/librobolectric-nativeruntime.so"
AGENT="$OUT/libredirect_agent.so"
CC=${CC:-$(command -v clang || command -v cc)}
command -v heif-convert > /dev/null || { echo "no heif-convert (libheif-examples)" >&2; exit 1; }

echo "== the redirect agent ($CC)"
"$CC" -shared -fPIC -O2 -Wall -Wextra -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" -o "$AGENT" \
    "$HERE/../aospgfx/redirect_agent.c" || exit 1

echo "== the classes: the test, windows/java/aosp_gfx, test/aospgfx's stand-ins for ART"
rm -rf "$OUT/classes" && mkdir -p "$OUT/classes"
"$JAVA_HOME/bin/javac" -encoding UTF-8 -nowarn -Xlint:-options --release 21 -d "$OUT/classes" -cp "$ALL" \
    $(find "$HERE/src" "$HERE/../../java/aosp_gfx" "$HERE/../aospgfx/src/android" "$HERE/../aospgfx/src/dalvik" \
        -name '*.java') || exit 1
echo "== android-all's BitmapFactory and ImageDecoder, redirected"
(cd "$OUT/classes" && "$JAVA_HOME/bin/jar" xf "$ALL" android/graphics/BitmapFactory.class \
    android/graphics/ImageDecoder.class 'android/graphics/ImageDecoder$ByteArraySource.class' \
    'android/graphics/ImageDecoder$ByteBufferSource.class' android/graphics/Bitmap.class) || exit 1
"$HERE/../../scripts/redirect_decoders.sh" "$JAVA_HOME/bin/java" "$OUT/classes" || exit 1
# And Bitmap's gain map natives, which the library lacks, as build_aosp_gfx.sh does (createScaledBitmap).
"$JAVA_HOME/bin/java" "$HERE/../../scripts/AospCallRedirect.java" "$OUT/classes" android/graphics/Bitmap \
    android/graphics/Bitmap nativeHasGainmap '(J)Z' com/winapk/aospgfx/MissingNatives \
    nativeExtractGainmap '(J)Landroid/graphics/Gainmap;' nativeSetGainmap '(JJ)V' || exit 1

rc=0
for mode in turned stored none; do
    echo "== HeifTest, the host decoder: $mode"
    "$JAVA_HOME/bin/java" -Xss4m -Dheiftest.codec=$mode -cp "$OUT/classes:$ALL" HeifTest "$HERE" "$AGENT" \
        "$NATIVE" "$DEPS/nativeruntime/icu/icudt68l.dat" 2>&1 | tee "$OUT/heif-$mode.log"
    [[ ${PIPESTATUS[0]} == 0 ]] && grep -q '^HEIFTEST OK' "$OUT/heif-$mode.log" || rc=1
done
[[ $rc == 0 ]] && echo "== passed" || echo "== FAILED"
exit $rc
