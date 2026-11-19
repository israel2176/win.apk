#!/usr/bin/env bash
# build_aosp_gfx.sh -- what WINAPK_AOSP_GFX=1 needs (windows/README.md, "Milestone 2: the runtime
# on AOSP graphics"; host/aosp_gfx.h), into a staged runtime:
#
#   <out>/lib/aosp-graphics.jar             AOSP 14's android.graphics.** (drawables, fonts, text,
#                                           animation, pdf; SurfaceTexture left out: Win.apk keeps
#                                           its own), the android.text classes that measure and lay
#                                           out (Layout, StaticLayout, BoringLayout, DynamicLayout,
#                                           MeasuredParagraph, PrecomputedText, TextLine,
#                                           Hyphenator, FontConfig), android.util.PathParser, the
#                                           classes of android-all these name that Win.apk's
#                                           framework lacks, and windows/java/aosp_gfx (the stand-ins
#                                           for ART and Robolectric the native runtime calls)
#   <out>/bin/robolectric-nativeruntime.dll Robolectric's native runtime (Skia, minikin, HarfBuzz...)
#   <out>/bin/aosp/icu/icudt68l.dat         its ICU data
#   <out>/bin/aosp/fonts/                   Android's fonts and fonts.xml
#   <out>/bin/aosp/hyphen-data/             the hyphenation patterns
#   <out>/bin/licenses/aosp-graphics/       their licences (THIRD_PARTY_NOTICES.txt, section 2)
#
#   windows/scripts/build_aosp_gfx.sh <out> [deps dir]
#
# <out> is a runtime staged by build_java.sh (its lib/android-framework.jar decides which extra
# classes go in). The deps dir (default thirdparty/aosp-gfx, CI caches it) holds android-all
# 14-robolectric-10818077 and nativeruntime-dist-compat 1.0.18, fetched from Maven Central's
# Google mirror and checked against their SHA-256 by windows/test/aospgfx/fetch.sh.
# Needs a JDK (JAVA_HOME: javac, jar, jdeps), curl and unzip. The jar is on no class path unless
# WINAPK_AOSP_GFX=1; stage_runtime.sh runs this unless WINAPK_STAGE_AOSP_GFX=0.

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
OUT="${1:?usage: build_aosp_gfx.sh <staged runtime> [deps dir]}"
DEPS="${2:-$HERE/../../thirdparty/aosp-gfx}"
OUT="$(cd -- "$OUT" && pwd)"
mkdir -p "$DEPS"
DEPS="$(cd -- "$DEPS" && pwd)"
FRAMEWORK="$OUT/lib/android-framework.jar"
[[ -f "$FRAMEWORK" ]] || { echo "[aosp-gfx] no $FRAMEWORK: run build_java.sh first" >&2; exit 1; }

if [[ -n "${JAVA_HOME:-}" ]]; then
    JAVAC="$JAVA_HOME/bin/javac" JAR="$JAVA_HOME/bin/jar" JDEPS="$JAVA_HOME/bin/jdeps"
else
    JAVAC=javac JAR=jar JDEPS=jdeps
fi
# javac reads an @file itself, so on MSYS2 (which translates only arguments) the paths in it
# are Windows paths, as build_java.sh writes them.
native_path() { if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }

"$HERE/../test/aospgfx/fetch.sh" "$DEPS" windows
ALL="$DEPS/android-all-14.jar"
NR="$DEPS/nativeruntime"

WORK="$OUT/.build/aosp-gfx"
CLASSES="$WORK/classes"
rm -rf "$WORK"
mkdir -p "$CLASSES"

echo "[aosp-gfx] AOSP's classes from android-all 14"
unzip -Z1 "$ALL" > "$WORK/all.txt"
# The text classes go with Paint (measuring and drawing text must agree), FontConfig with
# SystemFonts; .uau files are Robolectric's annotations, not classes.
grep -E '^(android/graphics/.*|android/text/(Layout|StaticLayout|BoringLayout|DynamicLayout|MeasuredParagraph|PrecomputedText|TextLine|Hyphenator|FontConfig)([$].*)?|android/util/PathParser([$].*)?)\.class$' \
    "$WORK/all.txt" | grep -v '^android/graphics/SurfaceTexture' > "$WORK/take.txt"

# What those classes name that Win.apk's framework (and the JDK) does not have: from android-all
# too, until nothing is missing, so a class loads instead of failing on a NoClassDefFoundError.
# Never these: com.android.internal.R holds AOSP's resource ids, not framework-res.apk's;
# android.app and android.animation are Win.apk's own (an inner class of AOSP's
# PropertyValuesHolder beside Win.apk's outer one, or the system-service AIDL behind
# IActivityManager, would be mixing two implementations).
extract() { (cd "$CLASSES" && xargs -d '\n' unzip -qo "$ALL" < "$1"); }
extract "$WORK/take.txt"
for round in 1 2 3 4 5 6 7 8; do
    "$JDEPS" -verbose:class -cp "$FRAMEWORK" "$CLASSES" 2> /dev/null | tr -d '\r' |
        awk '$NF == "found" && $(NF-1) == "not" { print $3 }' | sort -u |
        grep -vE '^(com\.android\.internal\.R([$]|$)|android\.app\.|android\.animation\.)' |
        tr . / | sed 's/$/.class/' | grep -xFf - "$WORK/all.txt" > "$WORK/more.txt" || true
    [[ -s "$WORK/more.txt" ]] || break
    echo "[aosp-gfx]   + $(wc -l < "$WORK/more.txt") classes they need that the framework lacks"
    extract "$WORK/more.txt"
done

echo "[aosp-gfx] Win.apk's stand-ins (windows/java/aosp_gfx)"
find "$HERE/../java/aosp_gfx" -name '*.java' | sort | while read -r f; do native_path "$f"; done > "$WORK/src.txt"
"$JAVAC" -nowarn --release 21 -d "$CLASSES" -cp "$ALL" @"$WORK/src.txt"

rm -f "$OUT/lib/aosp-graphics.jar"
( cd "$CLASSES" && "$JAR" --create --file "$OUT/lib/aosp-graphics.jar" . )
echo "[aosp-gfx] $OUT/lib/aosp-graphics.jar: $(find "$CLASSES" -name '*.class' | wc -l) classes"

echo "[aosp-gfx] the native runtime, ICU's data, the fonts"
mkdir -p "$OUT/bin/aosp/icu" "$OUT/bin/licenses/aosp-graphics"
cp -f "$NR/native/windows/x86_64/robolectric-nativeruntime.dll" "$OUT/bin/"
cp -f "$NR/icu/icudt68l.dat" "$OUT/bin/aosp/icu/"
rm -rf "$OUT/bin/aosp/fonts" "$OUT/bin/aosp/hyphen-data"
cp -RL "$NR/fonts" "$OUT/bin/aosp/fonts"
rm -f "$OUT/bin/aosp/fonts/fontsInSdk.txt"  # Robolectric's own list, not a font
cp -RL "$NR/hyphen-data" "$OUT/bin/aosp/hyphen-data"
cp -f "$HERE/../third_party/aosp_gfx_licenses/"*.txt "$OUT/bin/licenses/aosp-graphics/"
rm -rf "$WORK"
du -sh "$OUT/lib/aosp-graphics.jar" "$OUT/bin/robolectric-nativeruntime.dll" "$OUT/bin/aosp" | sed 's/^/[aosp-gfx] /'
