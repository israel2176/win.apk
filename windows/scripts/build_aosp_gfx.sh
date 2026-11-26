#!/usr/bin/env bash
# build_aosp_gfx.sh -- what AOSP's graphics, the only graphics the framework has, need (windows/README.md,
# "Milestone 2: the runtime on AOSP graphics"; host/aosp_gfx.h), into a staged runtime:
#
#   <out>/lib/aosp-graphics.jar             AOSP 14's android.graphics.** (drawables, fonts, text,
#                                           animation, pdf; SurfaceTexture and BLASTBufferQueue
#                                           left out: Win.apk keeps its own), the android.text
#                                           classes that measure and lay
#                                           out (Layout, StaticLayout, BoringLayout, DynamicLayout,
#                                           MeasuredParagraph, PrecomputedText, TextLine,
#                                           Hyphenator, FontConfig), android.util.PathParser, the
#                                           classes of android-all these name that Win.apk's
#                                           framework lacks, and windows/java/aosp_gfx (the stand-ins
#                                           for ART and Robolectric the native runtime calls, and
#                                           the API 35 methods added to AOSP 14's classes)
#   <out>/bin/aosp-hwui.dll                 the graphics DLL: AOSP 14's HWUI (its render thread and GL
#                                           pipeline), Skia, minikin, HarfBuzz, FreeType, ICU, built
#                                           from source by windows/hwui_build/build.sh
#   <out>/bin/aosp/icu/icudt72l.dat         its ICU data (ICU 72's), without the trees it cannot open
#   <out>/bin/aosp/fonts/                   Android's fonts.xml and the fonts it names, but Noto CJK
#                                           (Windows' own CJK fonts stand in, HostCjkFonts)
#   <out>/bin/aosp/hyphen-data/             the hyphenation patterns
#   <out>/bin/licenses/aosp-graphics/       their licences (THIRD_PARTY_NOTICES.txt, section 2)
#
#   windows/scripts/build_aosp_gfx.sh <out> [deps dir]
#
# <out> is a runtime staged by build_java.sh (its lib/android-framework.jar decides which extra
# classes go in). The deps dir (default thirdparty/aosp-gfx, CI caches it) holds android-all
# 14-robolectric-10818077 and nativeruntime-dist-compat 1.0.18, fetched from Maven Central's
# Google mirror and checked against their SHA-256 by windows/test/aospgfx/fetch.sh.
# The graphics DLL and ICU's data come from windows/hwui_build/build.sh's output folder:
# WINAPK_HWUI_DIR (default thirdparty/aosp-hwui/b, where CI builds it); Robolectric's distribution
# gives the fonts and the hyphenation patterns only.
# Needs a JDK (JAVA_HOME: javac, jar, jdeps), curl and unzip. The jar is first on the class path of every
# JVM with the framework, which cannot start without these files (host/aosp_gfx.h); stage_runtime.sh runs
# this always.
#
# Every file here is one shared copy per install, never per app, and only what the native runtime
# can read is staged (windows/README.md, "Footprint"): what is left out could not change a pixel.
# windows/scripts/footprint_check.py holds the stage and the installer to a budget.

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
OUT="${1:?usage: build_aosp_gfx.sh <staged runtime> [deps dir]}"
DEPS="${2:-$HERE/../../thirdparty/aosp-gfx}"
OUT="$(cd -- "$OUT" && pwd)"
mkdir -p "$DEPS"
DEPS="$(cd -- "$DEPS" && pwd)"
FRAMEWORK="$OUT/lib/android-framework.jar"
[[ -f "$FRAMEWORK" ]] || { echo "[aosp-gfx] no $FRAMEWORK: run build_java.sh first" >&2; exit 1; }
# The closure below is what the whole framework lacks: a framework stage_runtime.sh has already
# dropped the classes AOSP's resources and views shadow from would make it take those too.
# By the listing, not zipinfo's exit code (MSYS2's answers 0 for a name that is not there); no grep -q,
# whose early exit would kill unzip with SIGPIPE under pipefail.
if unzip -Z1 "$FRAMEWORK" | grep -x "META-INF/winapk-aosp-shadowed.txt" > /dev/null; then
    echo "[aosp-gfx] $FRAMEWORK lacks the classes stage_runtime.sh dropped: run build_java.sh again" >&2
    exit 1
fi

if [[ -n "${JAVA_HOME:-}" ]]; then
    JAVA="$JAVA_HOME/bin/java" JAVAC="$JAVA_HOME/bin/javac" JAR="$JAVA_HOME/bin/jar" JDEPS="$JAVA_HOME/bin/jdeps"
else
    JAVA=java JAVAC=javac JAR=jar JDEPS=jdeps
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
# The one list of what is taken, which build_java.sh compiles the framework against too.
# shellcheck source=aosp_gfx_take.sh
source "$HERE/aosp_gfx_take.sh"
aosp_gfx_take "$WORK/all.txt" > "$WORK/take.txt"

# What those classes name that Win.apk's framework (and the JDK) does not have: from android-all
# too, until nothing is missing, so a class loads instead of failing on a NoClassDefFoundError.
# Never these: com.android.internal.R holds android-14's resource ids, not the shipped table's
# (AospResIds.java writes the part the classes read, below);
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
# jdeps leaves out what a class names in its own package, unless -filter:none; so a second pass
# for those, in the packages taken above only: the text classes' helpers in android.text
# (AutoGrowArray, SpanSet, AndroidBidi, PackedIntVector...) and nested classes of a class taken
# (GradientColor's factory; com.android.internal.util's FastPrintWriter$DummyWriter, without which
# FastPrintWriter did not link). A nested class of one of Win.apk's own (android.view.View's
# AttachInfo, which ThreadedRenderer names) is never taken.
for round in 1 2 3 4; do
    (cd "$CLASSES" && find . -name '*.class' | sed 's#^[.]/##') > "$WORK/have.txt"
    "$JDEPS" -verbose:class -filter:none -cp "$FRAMEWORK" "$CLASSES" 2> /dev/null | tr -d '\r' |
        awk '$NF == "found" && $(NF-1) == "not" { f = $1; t = $3; sub(/[.][^.]*$/, "", f);
             p = t; sub(/[.][^.]*$/, "", p); if (f == p) print t }' | sort -u |
        grep -E '^(android[.](text|graphics|content[.]res|util)|com[.]android[.]internal[.]util)[.]' | tr . / |
        awk -v have="$WORK/have.txt" 'BEGIN { while ((getline l < have) > 0) h[l] = 1 }
             { o = $0; sub(/[$].*/, "", o); if (o == $0 || h[o ".class"]) print $0 ".class" }' |
        grep -xFf - "$WORK/all.txt" > "$WORK/more.txt" || true
    [[ -s "$WORK/more.txt" ]] || break
    echo "[aosp-gfx]   + $(wc -l < "$WORK/more.txt") classes of their own packages"
    extract "$WORK/more.txt"
done

# AOSP's drawables read their XML through com.android.internal.R.styleable, and AdaptiveIconDrawable
# has a private string's id compiled in: both linked to the platform table Win.apk ships
# (AospResIds.java says how). That table is android-all 14's own resources.arsc, which
# build_aosp_res.sh ships as bin/aosp/framework-res-34.apk, so it is read from android-all here:
# every id comes out as android-14 has it (aosp-resources.jar's own com.android.internal.R and
# AdaptiveIconDrawable, first on the class path, shadow these two anyway).
FWRES="$ALL"
echo "[aosp-gfx] com.android.internal.R and private ids, from android-all 14's resources.arsc"
mkdir -p "$WORK/gen"
"$JAVA" "$(native_path "$HERE/AospResIds.java")" "$(native_path "$ALL")" "$(native_path "$CLASSES")" \
    "$(native_path "$FWRES")" "$(native_path "$WORK/gen/R.java")"

# Where AOSP's code relies on what ART does and HotSpot does not, or on a native the native runtime
# lacks, its call goes to a Win.apk method of the same signature (windows/java/aosp_gfx, below;
# AospCallRedirect.java says how): Font$Builder reads a font the APK holds compressed into a direct
# buffer's array(); the library has no Typeface.nativeRegisterLocaleList (Typeface's static initializer
# calls it for the families of /system/etc/fonts.xml), no nPunchHole (SurfaceView's punchHole) and no
# Bitmap.nativeHasGainmap / nativeExtractGainmap / nativeSetGainmap (API 34's HDR gain maps:
# createScaledBitmap and every createBitmap with a matrix ask for one, BitmapFactory clears an inBitmap's);
# AdaptiveIconDrawable reads its mask
# through the Application, which the app's icon at start is built before (IconMaskThread).
echo "[aosp-gfx] calls redirected to Win.apk's"
"$JAVA" "$(native_path "$HERE/AospCallRedirect.java")" "$(native_path "$CLASSES")" 'android/graphics/fonts/Font$Builder' \
    'android/graphics/fonts/Font$Builder' createBuffer '(Landroid/content/res/AssetManager;Ljava/lang/String;ZI)Ljava/nio/ByteBuffer;' \
    com/winapk/aospgfx/FontBuffers
"$JAVA" "$(native_path "$HERE/AospCallRedirect.java")" "$(native_path "$CLASSES")" \
    android/graphics/drawable/AdaptiveIconDrawable android/app/ActivityThread currentActivityThread \
    '()Landroid/app/ActivityThread;' com/winapk/aospgfx/IconMaskThread
for canvas in android/graphics/BaseCanvas android/graphics/BaseRecordingCanvas; do
    "$JAVA" "$(native_path "$HERE/AospCallRedirect.java")" "$(native_path "$CLASSES")" "$canvas" "$canvas" \
        nPunchHole '(JFFFFFFF)V' com/winapk/aospgfx/MissingNatives
done
"$JAVA" "$(native_path "$HERE/AospCallRedirect.java")" "$(native_path "$CLASSES")" android/graphics/Typeface \
    android/graphics/Typeface nativeRegisterLocaleList '(Ljava/lang/String;)V' com/winapk/aospgfx/MissingNatives
"$JAVA" "$(native_path "$HERE/AospCallRedirect.java")" "$(native_path "$CLASSES")" android/graphics/Bitmap \
    android/graphics/Bitmap nativeHasGainmap '(J)Z' com/winapk/aospgfx/MissingNatives \
    nativeExtractGainmap '(J)Landroid/graphics/Gainmap;' nativeSetGainmap '(JJ)V'
# The native runtime has no HEIF codec (a device's comes from its media stack): BitmapFactory's and
# ImageDecoder's decoding natives go through HostImageDecode, which calls AOSP's and, when that fails on a
# HEIF or AVIF file, decodes it with the host's WIC (NativeImageCodec), as Java2D's graphics did.
"$HERE/redirect_decoders.sh" "$JAVA" "$(native_path "$CLASSES")"

echo "[aosp-gfx] Win.apk's stand-ins (windows/java/aosp_gfx)"
{ find "$HERE/../java/aosp_gfx" -name '*.java'; echo "$WORK/gen/R.java"; } | sort |
    while read -r f; do native_path "$f"; done > "$WORK/src.txt"
"$JAVAC" -nowarn --release 21 -d "$CLASSES" -cp "$ALL" @"$WORK/src.txt"

# The methods AOSP 14's classes lack that apps built for the runtime's SDK level (35) call, and that
# Win.apk's own framework calls on a class AOSP's replaces: each public static method of these helpers
# becomes a method of its first parameter's class (AospAddMethods.java says how).
echo "[aosp-gfx] methods added to AOSP's classes"
"$JAVA" "$(native_path "$HERE/AospAddMethods.java")" "$(native_path "$CLASSES")" \
    com/winapk/aospgfx/Api35 com/winapk/aospgfx/FrameworkMembers

rm -f "$OUT/lib/aosp-graphics.jar"
( cd "$CLASSES" && "$JAR" --create --file "$OUT/lib/aosp-graphics.jar" . )
echo "[aosp-gfx] $OUT/lib/aosp-graphics.jar: $(find "$CLASSES" -name '*.class' | wc -l) classes"

echo "[aosp-gfx] the native runtime, ICU's data, the fonts"
mkdir -p "$OUT/bin/aosp/icu" "$OUT/bin/licenses/aosp-graphics"
# The DLL carries its COFF symbol table (3.3 MB): read by no loader and by nothing in Win.apk (its
# exports, which the JVM binds by, stay). Only LLVM's strip (CLANG64's strip is LLVM's): it leaves
# the sections as lld wrote them, where GNU strip writes the PE file anew.
HWUI="${WINAPK_HWUI_DIR:-$HERE/../../thirdparty/aosp-hwui/b}"
for f in aosp-hwui.dll icudt72l.dat; do
    [[ -f "$HWUI/$f" ]] || { echo "[aosp-gfx] no $HWUI/$f: build it with windows/hwui_build/build.sh" >&2; exit 1; }
done
rm -f "$OUT/bin/robolectric-nativeruntime.dll" "$OUT/bin/aosp/icu/icudt68l.dat"
DLL="$OUT/bin/aosp-hwui.dll"
STRIP=$(command -v llvm-strip || true)
if [[ -z "$STRIP" ]] && command -v strip > /dev/null && strip --version 2>&1 | grep -q LLVM; then STRIP=strip; fi
if [[ -z "$STRIP" ]] || ! "$STRIP" -o "$DLL" "$HWUI/aosp-hwui.dll"; then
    echo "[aosp-gfx]   no LLVM strip: the native runtime staged with its symbol table" >&2
    cp -f "$HWUI/aosp-hwui.dll" "$DLL"
fi
# ICU's data, read by the DLL's ICU4C and by AOSP's ICU4J in this jar (android.icu: the apps' ICU,
# whose data path android.icu.platform.AndroidDataFiles gives), without the charset converters
# (".cnv" and "cnvalias": neither reads them; Java's charsets are the JDK's). ICU4J needs the rest: the
# time zone names, units, spelled-out numbers (the Hebrew calendar's dates are written with them) and
# transliterators the DLL alone did not. 25.8 MB -> about 22 MB (16.4 MB when the DLL was the only reader).
"$JAVA" "$(native_path "$HERE/TrimIcuData.java")" "$(native_path "$HWUI/icudt72l.dat")" \
    "$(native_path "$OUT/bin/aosp/icu/icudt72l.dat")" \
    '*.cnv' cnvalias.icu
rm -rf "$OUT/bin/aosp/fonts" "$OUT/bin/aosp/hyphen-data"
# Only the fonts fonts.xml names: SystemFonts opens nothing else, and a device has nothing else in
# /system/fonts either. The dist carries more, for Robolectric's older SDKs: NotoSansCJK's six other
# weights (107 MB; a device has only the Regular, and minikin makes the bold), DroidSansFallback,
# NanumGothic, Lato, Rubik, Arvo... 160 MB -> 47 MB.
# Nor Noto CJK Regular (19 MB, left out for size: Hebrew and Latin stay Android's, CJK leaves the
# install): fonts.xml still names it, and at run time its four families (zh-Hans, zh-Hant, ja, ko) are
# Windows' own CJK fonts instead (AospGfx.materializeSystemFonts, HostCjkFonts). 47 MB -> 29 MB.
WINDOWS_FALLBACK='NotoSansCJK-Regular.ttc'
mkdir -p "$OUT/bin/aosp/fonts"
cp -f "$NR/fonts/fonts.xml" "$OUT/bin/aosp/fonts/"
grep -oE '[A-Za-z0-9_.-]+[.](ttf|ttc|otf)' "$NR/fonts/fonts.xml" | sort -u | grep -vxF "$WINDOWS_FALLBACK" |
    while read -r f; do
        if [[ -f "$NR/fonts/$f" ]]; then cp -fL "$NR/fonts/$f" "$OUT/bin/aosp/fonts/"; fi
    done
cp -RL "$NR/hyphen-data" "$OUT/bin/aosp/hyphen-data"
cp -f "$HERE/../third_party/aosp_gfx_licenses/"*.txt "$OUT/bin/licenses/aosp-graphics/"
rm -rf "$WORK"
du -sh "$OUT/lib/aosp-graphics.jar" "$OUT/bin/aosp-hwui.dll" "$OUT/bin/aosp" | sed 's/^/[aosp-gfx] /'
