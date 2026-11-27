#!/usr/bin/env bash
# AOSP 14's own activity lifecycle on a plain JVM (docs/research/aosp-app.md, step A1): builds
# lib/aosp-app.jar over a staged runtime, the host library (plainjvm_host.cpp: the host natives and
# the host's graphics redirect) and AospAppLifecycleTest.java, and runs it; then MagnifierTest.java (the
# text magnifier's popup as a window's child layers) on the same harness; then StateRestoreTest.java (the
# task kept across the end of the app's process), one run per process.
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
# magnifier's checks and its own "ALL PASSED" or "FAILED", then each restore run's checks and a last
# "ALL PASSED" or "FAILED: <run>".
#
# AOSPAPP_TESTS (a test hook: the parts to run, of "lifecycle magnifier restore", all by default) lets CI
# run the restore test in a step of its own, which fails the run, beside the informational ones.
set -euo pipefail
TESTS=" ${AOSPAPP_TESTS:-lifecycle magnifier restore} "

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
"$JAVA_HOME/bin/javac" -nowarn -cp "$CP" -d "$OUT/classes" "$HERE/AospAppLifecycleTest.java" "$HERE/MagnifierTest.java" \
    "$HERE/StateRestoreTest.java"
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
        -Dandroid.icu.impl.ICUBinary.dataPath="$STAGE/bin/aosp/icu" \
        -Dwinapk.test.gfx.runtime="$RUNTIME" -Dwinapk.test.host="$OUT/libplainjvm_host.so" \
        -cp "$CP:$OUT/classes" "$@"
}
if [[ "$TESTS" == *" lifecycle "* ]]; then
    run_test AospAppLifecycleTest
fi
# The text magnifier's popup: child layers of a window, composed over it (MagnifierTest.java).
if [[ "$TESTS" == *" magnifier "* ]]; then
    echo "== the magnifier"
    run_test MagnifierTest
fi
[[ "$TESTS" == *" restore "* ]] || exit 0
# An app's task across the end of its process (StateRestoreTest.java): each run is one process of the
# app, over one saved state. A kill, the session's end or a crash keeps the task; Back out of it, the
# window's close (the task dismissed) and an update of the app drop it; one that does not open is none.
echo "== the task across the process's end"
STATE="$OUT/task-state.parcel"
rm -f "$STATE"
restore_runs() {
    local step mode extra out
    for step in "$@"; do
        mode="${step%%:*}"
        extra=""
        [[ "$step" == *:* ]] && extra="${step#*:}"
        out="$(run_test StateRestoreTest "$mode" "$STATE" $extra 2>&1)" || { echo "$out" | tail -n 40; echo "FAILED: $step"; exit 1; }
        echo "$out" | grep -E '^(==|  )' || true
        echo "$out" | grep -qE '^(ALL PASSED|KILLED)' || { echo "$out" | tail -n 40; echo "FAILED: $step did not finish"; exit 1; }
    done
}
# Killed in the background: the task back on Detail; Back out of Root then drops it.
restore_runs background expect-detail expect-fresh
# Killed in front: only Root, stopped, comes back; the window's close drops the task.
restore_runs foreground expect-root expect-fresh
# The Windows session's end: Detail stopped, and back on the next start.
restore_runs shutdown expect-detail
# A crash with the task behind: its top is not kept.
restore_runs crash expect-root
# An update of the app between the two processes: nothing of the older version's task.
restore_runs background expect-fresh:updated
# A saved task that does not open (not sealed for this Windows user): no saved task.
restore_runs background expect-fresh:tampered
echo "ALL PASSED"
