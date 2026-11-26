#!/usr/bin/env bash
# AOSP 14's own activity lifecycle on a plain JVM (docs/research/aosp-app.md, step A1): builds
# lib/aosp-app.jar over a staged runtime, the host library (plainjvm_host.cpp: the host natives and
# the host's graphics redirect) and AospAppLifecycleTest.java, and runs it; then MagnifierTest.java (the
# text magnifier's popup as a window's child layers) on the same harness.
#
#   windows/test/aospapp/run.sh <stage> <deps> [out] [full android-framework.jar]
#
# <stage> has the four other jars and bin/aosp/ (build_java.sh, build_aosp_gfx.sh, build_aosp_res.sh,
# build_aosp_views.sh, or stage_runtime.sh; CI: the runtime-stage artifact). A stage_runtime.sh
# stage's android-framework.jar lacks the classes AOSP's resources and views shadow: then the last
# argument is the whole jar (<cmake build dir>/java/android-framework.jar; CI: winapk-binaries'),
# which build_aosp_app.sh compiles and links against; the test runs on the stage's, as an app would.
# <deps> is the folder windows/test/aospgfx/fetch.sh <deps> linux fills (android-all 14,
# Robolectric's native runtime for this platform, ICU's data); build_aosp_app.sh adds ASM. Linux; prints the
# callbacks and ends with "ALL PASSED" (exit 0) or the first difference from Android's order, then the
# magnifier's checks and its own "ALL PASSED" or "FAILED".
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
STAGE="$(cd -- "${1:?usage: run.sh <stage> <deps> [out] [full android-framework.jar]}" && pwd)"
DEPS="$(cd -- "${2:?usage: run.sh <stage> <deps> [out] [full android-framework.jar]}" && pwd)"
OUT="${3:-$STAGE/.build/aospapp-test}"
mkdir -p "$OUT"
OUT="$(cd -- "$OUT" && pwd)"
JAVA_HOME="${JAVA_HOME:-$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")}"
CXX="${CXX:-$(command -v clang++ || command -v c++)}"

"$HERE/../../scripts/build_aosp_app.sh" "$STAGE" "$DEPS" ${4:+"$4"}

NR="$DEPS/nativeruntime"
RUNTIME="$NR/native/linux/x86_64/librobolectric-nativeruntime.so"
[[ -f "$RUNTIME" ]] || { echo "no $RUNTIME: windows/test/aospgfx/fetch.sh $DEPS linux" >&2; exit 1; }

echo "== the host library ($CXX)"
"$CXX" -std=c++17 -O1 -shared -fPIC -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" \
    "$HERE/plainjvm_host.cpp" "$HERE/../../host/aosp_gfx_redirect.cpp" -o "$OUT/libplainjvm_host.so"

L="$STAGE/lib"
CP="$L/aosp-app.jar:$L/aosp-views.jar:$L/aosp-resources.jar:$L/aosp-graphics.jar:$L/android-framework.jar:$L/macapk-loader.jar"
echo "== the test"
rm -rf "$OUT/classes"
"$JAVA_HOME/bin/javac" -nowarn -cp "$CP" -d "$OUT/classes" "$HERE/AospAppLifecycleTest.java" "$HERE/MagnifierTest.java"
# As winapk-runtime starts an app's JVM: no verification of the app's and the framework's classes
# (a class AOSP's reach only on a path not taken may name one Win.apk lacks), AOSP's graphics and
# views on, the framework's resources and fonts from the stage, ICU's data from the native runtime's.
# jdk.unsupported patched as winapk-runtime patches it (sun.misc.Cleaner, which a HardwareRenderer's
# native allocation is freed by: a Magnifier's popup has one).
run_test() {
    MACAPK_LOG_LEVEL=7 timeout 120 "$JAVA_HOME/bin/java" -XX:+UnlockDiagnosticVMOptions -XX:-BytecodeVerificationRemote \
        --patch-module=jdk.unsupported="$L/jdk_unsupported_patch" \
        --enable-native-access=ALL-UNNAMED -Djava.awt.headless=true \
        -Dwinapk.aosp.gfx=1 -Dwinapk.aosp.res=1 -Dwinapk.aosp.views=1 \
        -Dwinapk.aosp.res.framework="$STAGE/bin/aosp/framework-res-34.apk" -Dwinapk.aosp.fonts="$STAGE/bin/aosp/fonts" \
        -Dicu.data.path="$NR/icu/icudt68l.dat" -Dicu.locale.default=en-US \
        -Dwinapk.test.gfx.runtime="$RUNTIME" -Dwinapk.test.host="$OUT/libplainjvm_host.so" \
        -cp "$CP:$OUT/classes" "$1"
}
run_test AospAppLifecycleTest
# The text magnifier's popup: child layers of a window, composed over it (MagnifierTest.java).
echo "== the magnifier"
run_test MagnifierTest
