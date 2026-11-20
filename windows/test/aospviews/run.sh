#!/usr/bin/env bash
# Builds and runs the AOSP resources-and-views spike (AospViews.java; docs/research/aosp-views.md):
# AOSP 14's own android.content.res and android.view / android.widget / android.text in a plain
# HotSpot JVM, resources on Robolectric's Java port of libandroidfw through a build-time rewrite of
# the natives (tools/RewriteNatives.java, tools/RemapJdk.java), graphics through
# windows/test/aospgfx's redirect agent and Robolectric's native runtime.
#
#   windows/test/aospviews/fetch.sh <deps>
#   windows/test/aospviews/run.sh <deps> <probe.apk> <out> [oracle.txt]
#
# <probe.apk> is windows/test/uiprobe's app (uiprobe/build.sh makes it, and oracle.txt and
# oracle-pictures/ beside it). Environment: JAVA_HOME a JDK 21 (javac, jni.h); CC the C compiler for
# the agent (clang, else cc); AOSPVIEWS_XMX the heap (default 512m); AOSPVIEWS_JAVA_OPTS more JVM
# options (e.g. -Xlog:class+load). Linux only for now (the agent and the .so; Windows would take
# aospgfx/run.sh's MSYS2 branch). Writes <out>/aospviews.log, rewrite.log, remap.log and a PNG per
# layout case.
set -uo pipefail

DEPS=${1:?usage: run.sh <deps> <probe.apk> <out> [oracle.txt]}
APK=${2:?usage: run.sh <deps> <probe.apk> <out> [oracle.txt]}
OUT=${3:?usage: run.sh <deps> <probe.apk> <out> [oracle.txt]}
ORACLE=${4:-}
HERE=$(cd "$(dirname "$0")" && pwd)
GFX=$HERE/../aospgfx
: "${JAVA_HOME:?set JAVA_HOME to a JDK 21}"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
DEPS=$(cd "$DEPS" && pwd)
ALL=$DEPS/android-all-14.jar
NATIVE=$DEPS/nativeruntime/native/linux/x86_64/librobolectric-nativeruntime.so
AGENT=$OUT/libredirect_agent.so
CC=${CC:-$(command -v clang || command -v cc)}
JARS=$(ls "$DEPS"/jars/*.jar | grep -v '/asm-' | tr '\n' ':')
ASM=$(ls "$DEPS"/jars/asm-*.jar)

echo "== the redirect agent ($CC)"
"$CC" -shared -fPIC -O2 -Wall -Wextra -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" \
    -o "$AGENT" "$GFX/redirect_agent.c" || exit 1

echo "== the rewrite tool, the bridge, the stand-ins and the test"
rm -rf "$OUT/tools" "$OUT/classes" "$OUT/rewritten"
mkdir -p "$OUT/tools" "$OUT/classes" "$OUT/rewritten"
"$JAVA_HOME/bin/javac" -nowarn -d "$OUT/tools" -cp "$ASM" "$HERE/tools/RewriteNatives.java" "$HERE/tools/RemapJdk.java" || exit 1
# The stand-ins for ART (Log, Trace, SystemProperties: aospgfx's; VMRuntime and
# NativeAllocationRegistry: this folder's) and Robolectric's RuntimeEnvironment (windows/java/aosp_gfx).
"$JAVA_HOME/bin/javac" -encoding UTF-8 -nowarn -Xlint:-options --release 21 -d "$OUT/classes" \
    -cp "$ALL:$JARS" \
    $(find "$HERE/src" -name '*.java') \
    "$GFX/src/android/util/Log.java" "$GFX/src/android/os/Trace.java" "$GFX/src/android/os/SystemProperties.java" \
    "$HERE/../../java/aosp_gfx/org/robolectric/RuntimeEnvironment.java" || exit 1

# The natives of these classes get Java bodies: libandroidfw's (android.content.res) on Robolectric's
# port, and the system layer's (aospviews.sys: clocks, libcore's Os, Binder, the looper, EventLog),
# which in Win.apk stays its own.
echo "== AOSP's natives rewritten to Java bodies"
"$JAVA_HOME/bin/java" -cp "$OUT/tools:$ASM" RewriteNatives "$ALL" "$OUT/classes" org.robolectric.shadows \
    "$OUT/rewritten" android.content.res.AssetManager android.content.res.ApkAssets \
    android.content.res.XmlBlock android.content.res.StringBlock \
    android.os.SystemClock:aospviews.sys android.system.OsConstants:aospviews.sys \
    android.os.Binder:aospviews.sys com.android.internal.os.BinderInternal:aospviews.sys \
    libcore.io.Linux:aospviews.sys android.util.EventLog:aospviews.sys android.os.MessageQueue:aospviews.sys \
    | tee "$OUT/rewrite.log" || exit 1
echo "== calls to java.* members only ART has (RemapJdk), in android.* and com.android.internal.*"
"$JAVA_HOME/bin/java" -Xmx256m -cp "$OUT/tools:$ASM" RemapJdk "$ALL" "$OUT/classes" aospviews.sys.Art \
    "$OUT/rewritten" android/ com/android/internal/ > "$OUT/remap.log" || exit 1
tail -n 1 "$OUT/remap.log"

# First on the class path: the rewritten classes, then the stand-ins and the bridge, then android-all
# (every other android.* class, unchanged), then Robolectric's port of libandroidfw and its jars.
CP="$OUT/rewritten:$OUT/classes:$ALL:$JARS"
echo "== AospViews (java -Xmx${AOSPVIEWS_XMX:-512m})"
"$JAVA_HOME/bin/java" -Xmx"${AOSPVIEWS_XMX:-512m}" -Xss4m ${AOSPVIEWS_JAVA_OPTS:-} -cp "$CP" AospViews "$AGENT" "$NATIVE" \
    "$DEPS/nativeruntime/icu/icudt68l.dat" "$DEPS/nativeruntime/fonts" "$ALL" "$APK" "$OUT" $ORACLE 2>&1 \
    | tee "$OUT/aospviews.log"
rc=${PIPESTATUS[0]}
if [[ $rc != 0 ]] || ! grep -q '^AOSPVIEWS OK' "$OUT/aospviews.log"; then
    echo "== FAILED (exit $rc); everything is in $OUT/aospviews.log"
    exit 1
fi
echo "== passed"
