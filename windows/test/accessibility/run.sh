#!/usr/bin/env bash
# What a screen reader is given of an app's views, on AOSP 14's own views on a plain JVM:
# AccessibilityTreeTest.java (HostAccessibility and com.winapk.aospviews.AccessibilityTree, the
# host's role played by the test), then snapshot_check.cpp (the last snapshot read by the host's own
# model, windows/host/uia_tree.h).
#
#   windows/test/accessibility/run.sh <stage> <deps> [out] [full android-framework.jar]
#
# <stage> and <deps> as windows/test/aospapp/run.sh takes them (it builds lib/aosp-app.jar into the
# stage; this builds it too when the stage lacks it). A11Y_CLASSES: a folder of classes put first on
# the class path (a local run's freshly compiled HostAccessibility, AccessibilityTree...). Linux; ends
# with "ALL PASSED" twice (exit 0), else names each failure.
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
STAGE="$(cd -- "${1:?usage: run.sh <stage> <deps> [out] [full android-framework.jar]}" && pwd)"
DEPS="$(cd -- "${2:?usage: run.sh <stage> <deps> [out] [full android-framework.jar]}" && pwd)"
OUT="${3:-$STAGE/.build/a11y-test}"
mkdir -p "$OUT"
OUT="$(cd -- "$OUT" && pwd)"
JAVA_HOME="${JAVA_HOME:-$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")}"
CXX="${CXX:-$(command -v clang++ || command -v c++)}"

[[ -f "$STAGE/lib/aosp-app.jar" ]] || "$HERE/../../scripts/build_aosp_app.sh" "$STAGE" "$DEPS" ${4:+"$4"}

NR="$DEPS/nativeruntime"
RUNTIME="$NR/native/linux/x86_64/librobolectric-nativeruntime.so"
[[ -f "$RUNTIME" ]] || { echo "no $RUNTIME: windows/test/aospgfx/fetch.sh $DEPS linux" >&2; exit 1; }

echo "== the host library and the snapshot check ($CXX)"
"$CXX" -std=c++17 -O1 -shared -fPIC -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" \
    "$HERE/../aospapp/plainjvm_host.cpp" "$HERE/a11y_natives.cpp" "$HERE/../../host/aosp_gfx_redirect.cpp" \
    -o "$OUT/liba11y_host.so"
"$CXX" -std=c++17 -O1 -Wall -Wextra -I"$HERE/../../host" "$HERE/snapshot_check.cpp" "$HERE/../../host/uia_tree.cpp" \
    -o "$OUT/snapshot_check"

L="$STAGE/lib"
CP="$L/aosp-app.jar:$L/aosp-views.jar:$L/aosp-resources.jar:$L/aosp-graphics.jar:$L/android-framework.jar:$L/macapk-loader.jar"
[[ -n "${A11Y_CLASSES:-}" ]] && CP="$A11Y_CLASSES:$CP"
echo "== the test"
rm -rf "$OUT/classes"
"$JAVA_HOME/bin/javac" -nowarn -encoding UTF-8 -cp "$CP" -d "$OUT/classes" "$HERE/AccessibilityTreeTest.java"
rm -f "$OUT/snapshot.txt"
# As windows/test/aospapp/run.sh starts its JVM (winapk-runtime's way).
MACAPK_LOG_LEVEL=7 timeout 120 "$JAVA_HOME/bin/java" -XX:+UnlockDiagnosticVMOptions -XX:-BytecodeVerificationRemote \
    --enable-native-access=ALL-UNNAMED -Djava.awt.headless=true \
    -Dwinapk.aosp.gfx=1 -Dwinapk.aosp.views=1 \
    -Dwinapk.aosp.res.framework="$STAGE/bin/aosp/framework-res-34.apk" -Dwinapk.aosp.fonts="$STAGE/bin/aosp/fonts" \
    -Dicu.data.path="$NR/icu/icudt68l.dat" -Dicu.locale.default=en-US \
    -Dwinapk.test.gfx.runtime="$RUNTIME" -Dwinapk.test.host="$OUT/liba11y_host.so" \
    -Dwinapk.test.snapshot="$OUT/snapshot.txt" \
    -cp "$CP:$OUT/classes" AccessibilityTreeTest
echo "== the host's model of the last snapshot"
"$OUT/snapshot_check" "$OUT/snapshot.txt"
