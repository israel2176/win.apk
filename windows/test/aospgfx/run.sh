#!/usr/bin/env bash
# Builds and runs the AOSP graphics test (AospGfx.java): AOSP 14's android.graphics / android.text
# drawing through Robolectric's native runtime (Skia, minikin, HarfBuzz) in HotSpot, bound by
# the redirect agent (redirect_agent.c). Milestone 1 of real Android graphics in Win.apk;
# windows/README.md, "AOSP graphics".
#
#   windows/test/aospgfx/fetch.sh <deps>
#   windows/test/aospgfx/run.sh <deps> <out> [bench W H warmup frames]
#
# Environment:
#   JAVA_HOME        a JDK (javac, jni.h); on MSYS2 a /c/... path
#   CC               the C compiler for the agent (default: clang, else cc)
#   AOSPGFX_JRE      the Java runtime to draw in (default: $JAVA_HOME); CI gives it the staged jre/
#   AOSPGFX_RUNTIME  winapk-runtime.exe: run inside Win.apk's own process (`--java-main`, the JVM
#                    created the way the runtime creates it) rather than with java(.exe)
#
# Works on Linux (the .so) and on Windows under MSYS2 CLANG64 (the .dll). Writes <out>/aospgfx.png,
# and <out>/aospgfx.log with everything printed. When the run fails it runs again with the agent's
# per-method log and, inside winapk-runtime, once more with plain java.exe, to tell the library's
# fault from the process's.
set -uo pipefail

DEPS=${1:?usage: run.sh <deps> <out> [bench W H warmup frames]}
OUT=${2:?usage: run.sh <deps> <out> [bench W H warmup frames]}
shift 2
HERE=$(cd "$(dirname "$0")" && pwd)
: "${JAVA_HOME:?set JAVA_HOME to a JDK}"
JRE=${AOSPGFX_JRE:-$JAVA_HOME}
mkdir -p "$OUT/classes"
OUT=$(cd "$OUT" && pwd)
DEPS=$(cd "$DEPS" && pwd)

case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*)
        WIN=1 SEP=';' EXE=.exe MD=win32
        AGENT="$OUT/redirect_agent.dll"
        NATIVE="$DEPS/nativeruntime/native/windows/x86_64/robolectric-nativeruntime.dll"
        host() { cygpath -w "$1"; }
        ;;
    *)
        WIN=0 SEP=':' EXE= MD=linux
        AGENT="$OUT/libredirect_agent.so"
        NATIVE="$DEPS/nativeruntime/native/linux/x86_64/librobolectric-nativeruntime.so"
        host() { echo "$1"; }
        ;;
esac
CC=${CC:-$(command -v clang || command -v cc)}

echo "== the redirect agent ($CC)"
flags=(-shared -O2 -Wall -Wextra -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/$MD")
[[ $WIN == 0 ]] && flags+=(-fPIC)
"$CC" "${flags[@]}" -o "$AGENT" "$HERE/redirect_agent.c" || exit 1
if [[ $WIN == 1 ]]; then
    # What the two libraries import: a DLL missing on the machine shows here, not as a bare
    # "Can't find dependent libraries".
    for f in "$AGENT" "$NATIVE"; do
        echo "-- $(basename "$f") imports:"
        (llvm-objdump -p "$f" 2>/dev/null || objdump -p "$f") | grep 'DLL Name' | sort -u | tr -s ' \t' ' '
    done
fi

echo "== the test classes"
"$JAVA_HOME/bin/javac$EXE" -encoding UTF-8 -nowarn -Xlint:-options --release 21 -d "$(host "$OUT/classes")" \
    -cp "$(host "$DEPS/android-all-14.jar")" $(find "$HERE/src" -name '*.java' | while read -r f; do host "$f"; done) \
    || exit 1

# The stand-ins first: they replace android-all's own Log, Trace, SystemProperties, VMRuntime and
# NativeAllocationRegistry, which are ART's.
CP="$(host "$OUT/classes")$SEP$(host "$DEPS/android-all-14.jar")"
ARGS=("$(host "$AGENT")" "$(host "$NATIVE")" "$(host "$DEPS/nativeruntime/icu/icudt68l.dat")"
      "$(host "$DEPS/nativeruntime/fonts")" "$(host "$OUT")" "$@")

run() {  # how: runtime | java
    if [[ $1 == runtime ]]; then
        echo "== in Win.apk's process: $(basename "$AOSPGFX_RUNTIME") --java-main --jdk $(host "$JRE")"
        "$AOSPGFX_RUNTIME" --java-main --jdk "$(host "$JRE")" --cp "$CP" AospGfx "${ARGS[@]}"
    else
        echo "== with $(host "$JRE/bin/java$EXE")"
        "$JRE/bin/java$EXE" -Xss4m -cp "$CP" AospGfx "${ARGS[@]}"
    fi
}

how=java
[[ -n "${AOSPGFX_RUNTIME:-}" ]] && how=runtime
run $how 2>&1 | tee "$OUT/aospgfx.log"
rc=${PIPESTATUS[0]}
if [[ $rc != 0 ]] || ! grep -q '^AOSPGFX OK' "$OUT/aospgfx.log"; then
    echo "== FAILED (exit $rc). Again with every skipped native and missing class listed:"
    AGENT_VERBOSE=1 AGENT_REGLOG="$(host "$OUT/registered.txt")" run $how > "$OUT/aospgfx-verbose.log" 2>&1
    echo "exit $?; the last 80 lines (all in aospgfx-verbose.log, bound methods in registered.txt):"
    tail -n 80 "$OUT/aospgfx-verbose.log"
    if [[ $how == runtime ]]; then
        echo "== and with plain java.exe of the same runtime, outside Win.apk's process:"
        run java > "$OUT/aospgfx-java.log" 2>&1
        echo "exit $?"
        tail -n 40 "$OUT/aospgfx-java.log"
    fi
    for f in hs_err_pid*.log "$OUT"/hs_err_pid*.log; do
        [[ -f "$f" ]] && { echo "== $f"; head -n 60 "$f"; cp "$f" "$OUT/" 2>/dev/null; }
    done
    exit 1
fi
echo "== passed"
