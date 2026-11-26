#!/usr/bin/env bash
# build_aosp_res.sh -- what AOSP's own resources, the only ones apps run on, need (host/aosp_gfx.h;
# docs/research/aosp-views.md, step R1; windows/README.md, "AOSP resources"), into a staged runtime:
#
#   <out>/lib/aosp-resources.jar          AOSP 14's android.content.res.** (Resources, ResourcesImpl,
#                                         AssetManager, ApkAssets, TypedArray, XmlBlock, ColorStateList
#                                         and ComplexColor inflation...), android.app.WindowConfiguration
#                                         (Configuration's), com.android.internal.R (the ids of the table
#                                         below), the classes of android-all these name that Win.apk's
#                                         framework lacks; Robolectric's Java port of libandroidfw
#                                         (org.robolectric.res.android) with the few classes of its own
#                                         it needs; and windows/java/aosp_res: the bodies of the natives
#                                         on that port, the backend (AospRes) and the stand-ins
#   <out>/bin/aosp/framework-res-34.apk   the framework's resources at the classes' level: android-all
#                                         14's resources.arsc, res/ and manifest, as an APK
#
#   windows/scripts/build_aosp_res.sh <out> [deps dir]
#
# <out> is a runtime staged by build_java.sh and build_aosp_gfx.sh (AOSP's resources run with AOSP's
# graphics: the host turns this off when those are off). The deps dir (default thirdparty/aosp-gfx,
# shared with build_aosp_gfx.sh) gets android-all 14 from windows/test/aospgfx/fetch.sh and, in
# jars/, Robolectric 4.16's `resources`, `utils` and `pluginapi` and ASM 9.8 (+ commons) for the
# rewrite, from Maven Central's Google mirror, checked against the SHA-256 below.
#
# At build time AospResRewrite.java (beside this script) gives the natives Java bodies, points
# AOSP's casts to XmlBlock.Parser at AttrSets, sends the calls Win.apk's classes cannot answer to
# Compat, and maps the port's Guava types to small stand-ins. stage_runtime.sh runs this after
# build_aosp_gfx.sh.

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
OUT="${1:?usage: build_aosp_res.sh <staged runtime> [deps dir]}"
DEPS="${2:-$HERE/../../thirdparty/aosp-gfx}"
OUT="$(cd -- "$OUT" && pwd)"
mkdir -p "$DEPS"
DEPS="$(cd -- "$DEPS" && pwd)"
FRAMEWORK="$OUT/lib/android-framework.jar"
LOADER="$OUT/lib/macapk-loader.jar"
GFX="$OUT/lib/aosp-graphics.jar"
for f in "$FRAMEWORK" "$LOADER"; do
    [[ -f "$f" ]] || { echo "[aosp-res] no $f: run build_java.sh first" >&2; exit 1; }
done
# The closure below is what the whole framework lacks: a framework stage_runtime.sh has already
# dropped the classes AOSP's resources and views shadow from would make it take those too.
# By the listing, not zipinfo's exit code (MSYS2's answers 0 for a name that is not there); no grep -q,
# whose early exit would kill unzip with SIGPIPE under pipefail.
if unzip -Z1 "$FRAMEWORK" | grep -x "META-INF/winapk-aosp-shadowed.txt" > /dev/null; then
    echo "[aosp-res] $FRAMEWORK lacks the classes stage_runtime.sh dropped: run build_java.sh again" >&2
    exit 1
fi
[[ -f "$GFX" ]] || { echo "[aosp-res] no $GFX: run build_aosp_gfx.sh first" >&2; exit 1; }

if [[ -n "${JAVA_HOME:-}" ]]; then
    JAVA="$JAVA_HOME/bin/java" JAVAC="$JAVA_HOME/bin/javac" JAR="$JAVA_HOME/bin/jar" JDEPS="$JAVA_HOME/bin/jdeps"
else
    JAVA=java JAVAC=javac JAR=jar JDEPS=jdeps
fi
native_path() { if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }
# A class path for the JDK's tools: ';' on Windows (MSYS2), ':' elsewhere.
if command -v cygpath > /dev/null 2>&1; then PS=';'; else PS=':'; fi
cp_of() { local out="" p; for p in "$@"; do out="${out:+$out$PS}$(native_path "$p")"; done; printf '%s' "$out"; }

"$HERE/../test/aospgfx/fetch.sh" "$DEPS" windows
ALL="$DEPS/android-all-14.jar"

MAVEN=https://maven-central.storage-download.googleapis.com/maven2
mkdir -p "$DEPS/jars"
fetch() {  # group-path artifact version sha256
    local f="$DEPS/jars/$2-$3.jar"
    if [[ -f "$f" ]] && echo "$4  $f" | sha256sum -c --status; then return 0; fi
    echo "[aosp-res] fetching $2-$3"
    curl -fL --retry 5 --retry-delay 5 --retry-all-errors -sS -o "$f.part" "$MAVEN/$1/$2/$3/$2-$3.jar"
    if ! echo "$4  $f.part" | sha256sum -c --status; then
        echo "[aosp-res] SHA-256 mismatch for $2-$3: got $(sha256sum "$f.part" | cut -d' ' -f1)" >&2
        exit 1
    fi
    mv "$f.part" "$f"
}
fetch org/robolectric resources 4.16 0d6abfd274d3c853a564ea23ce1c52d31d075281ae0e7d64bbbc1477b2ed7679
fetch org/robolectric utils 4.16 51940c4668ed065101c8a932c6b8a7efa7b546fd5a8e98180f40ac3840b27428
fetch org/robolectric pluginapi 4.16 ffdb38f7d0a667e10d0609472ec13792018a69ccaf52c776166e03c61eae6428
fetch org/ow2/asm asm 9.8 876eab6a83daecad5ca67eb9fcabb063c97b5aeb8cf1fca7a989ecde17522051
fetch org/ow2/asm asm-commons 9.8 3301a1c1cb4c59fcc5292648dac1d7c5aed4c0f067dfbe88873b8cdfe77404f4
PORT="$DEPS/jars/resources-4.16.jar"
RUTILS="$DEPS/jars/utils-4.16.jar"
PLUGINAPI="$DEPS/jars/pluginapi-4.16.jar"
ASM="$(cp_of "$DEPS/jars/asm-9.8.jar" "$DEPS/jars/asm-commons-9.8.jar")"

WORK="$OUT/.build/aosp-res"
CLASSES="$WORK/classes"
rm -rf "$WORK"
mkdir -p "$CLASSES"

echo "[aosp-res] AOSP's classes from android-all 14"
unzip -Z1 "$ALL" > "$WORK/all.txt"
# android.content.res whole; WindowConfiguration goes with Configuration (Win.apk's lacks what
# Configuration.setTo / updateFrom / diff call, and no Win.apk class uses a member AOSP's lacks);
# com.android.internal.R is android-all's own, matching the table staged below (and so first for
# aosp-graphics.jar's classes too, whose own R is generated for Win.apk's API 36 table).
# AdaptiveIconDrawable too: aosp-graphics.jar's copy has a private string id of Win.apk's API 36
# table compiled in (AospResIds.java); android-all's has the 34 table's, the one staged here.
# FastXmlSerializer and TypedXmlSerializer: Xml.newFastSerializer's (XmlUtils writes a map with it,
# SharedPreferencesImpl's file; AospResRewrite sends XmlUtils' calls of Xml's hidden members to Compat).
grep -E '^(android/content/res/.*|android/app/WindowConfiguration([$].*)?|com/android/internal/R([$].*)?|android/graphics/drawable/AdaptiveIconDrawable([$].*)?|com/android/internal/util/FastXmlSerializer|com/android/modules/utils/TypedXml(Serializer|PullParser))\.class$' \
    "$WORK/all.txt" > "$WORK/take.txt"
extract() { (cd "$CLASSES" && xargs -d '\n' unzip -qo "$1" < "$2"); }
extract "$ALL" "$WORK/take.txt"

# The port of libandroidfw, and what it names of Robolectric's own (Fs, PerfStatsCollector,
# Logger...). Guava is not taken: AospResRewrite maps it to windows/java/aosp_res's stand-ins.
unzip -Z1 "$PORT" | grep -E '^org/robolectric/res/android/.*\.class$' > "$WORK/port.txt"
extract "$PORT" "$WORK/port.txt"

# What those classes name that Win.apk's framework (with AOSP's graphics) and the JDK lack: from
# android-all, the port's jars, until nothing is missing (as build_aosp_gfx.sh). Never android.app
# beyond WindowConfiguration, nor android.animation: Win.apk's own. -filter:none, for what a class
# names in its own package (the port's Clock beside PerfStatsCollector); but a nested class only
# with its outer class here, never one of Win.apk's own classes (two implementations mixed). Nor
# libcore's own java.* classes (java.nio.NioUtils): no class loader but the JDK's defines a class of
# those packages ("Prohibited package name"), so one in the jar could never load.
for round in 1 2 3 4 5 6 7 8 9 10; do
    (cd "$CLASSES" && find . -name '*.class' | sed 's#^[.]/##') > "$WORK/have.txt"
    "$JDEPS" --multi-release 21 -verbose:class -filter:none -cp "$(cp_of "$GFX" "$FRAMEWORK" "$LOADER")" \
        "$CLASSES" 2> /dev/null |
        tr -d '\r' | awk '$NF == "found" && $(NF-1) == "not" { print $3 }' | sort -u |
        grep -vE '^(java\.|android\.app\.|android\.animation\.|com\.google\.|javax\.annotation\.|org\.jspecify\.)' |
        tr . / | awk -v have="$WORK/have.txt" 'BEGIN { while ((getline l < have) > 0) h[l] = 1 }
            { o = $0; sub(/[$].*/, "", o); if (o == $0 || h[o ".class"]) print $0 ".class" }' \
        > "$WORK/need.txt" || true
    n=0
    for src in "$ALL" "$PORT" "$RUTILS" "$PLUGINAPI"; do
        unzip -Z1 "$src" | grep -xFf "$WORK/need.txt" > "$WORK/more.txt" || true
        if [[ -s "$WORK/more.txt" ]]; then
            n=$((n + $(wc -l < "$WORK/more.txt")))
            extract "$src" "$WORK/more.txt"
        fi
    done
    [[ $n -gt 0 ]] || break
    echo "[aosp-res]   + $n classes they need that the framework lacks"
done

# AOSP's decoders hand an AssetManager.AssetInputStream's native Asset* straight to the native
# runtime (BitmapFactory.nativeDecodeAsset, ImageDecoder's AssetInputStreamSource...). Here an
# asset is the port's Java object, so the graphics classes that test for one are taken from
# aosp-graphics.jar into this jar (first on the class path, so they win) for the rewrite to make
# that test false: they read the stream as any other InputStream.
mkdir -p "$WORK/gfx"
(cd "$WORK/gfx" && unzip -qo "$GFX" 'android/graphics/*')
(cd "$WORK/gfx" && grep -rlF 'android/content/res/AssetManager$AssetInputStream' --include='*.class' . |
    sed 's#^[.]/##') > "$WORK/asset-readers.txt" || true
(cd "$WORK/gfx" && xargs -d '\n' cp --parents -t "$CLASSES" < "$WORK/asset-readers.txt")
echo "[aosp-res]   + $(wc -l < "$WORK/asset-readers.txt") graphics classes that read a native asset"

# AdaptiveIconDrawable is android-all's here (its private config_icon_mask id is this table's,
# above), so it needs what build_aosp_gfx.sh does to aosp-graphics.jar's copy: its constructor reads
# the mask through the Application, which Win.apk's start renders the app's launcher icon before
# (Boot.writeLauncherIcon); its currentActivityThread calls go to aosp-graphics.jar's
# IconMaskThread, which answers null until there is an Application (AOSP's Resources.getSystem()
# branch). Without it every adaptive launcher icon threw with the switch on.
"$JAVA" "$(native_path "$HERE/AospCallRedirect.java")" "$(native_path "$CLASSES")" \
    android/graphics/drawable/AdaptiveIconDrawable android/app/ActivityThread currentActivityThread \
    '()Landroid/app/ActivityThread;' com/winapk/aospgfx/IconMaskThread

echo "[aosp-res] windows/java/aosp_res"
find "$HERE/../java/aosp_res" -name '*.java' | sort | while read -r f; do native_path "$f"; done > "$WORK/src.txt"
# AOSP's classes (android-all) before Win.apk's of the same name: the backend is written against
# AOSP's API; com.macapk.* comes from Win.apk's jars.
"$JAVAC" -nowarn -encoding UTF-8 --release 21 -d "$CLASSES" \
    -cp "$(cp_of "$CLASSES" "$ALL" "$PORT" "$FRAMEWORK" "$LOADER")" @"$WORK/src.txt"

echo "[aosp-res] the rewrite (AospResRewrite.java)"
"$JAVA" -cp "$ASM" "$(native_path "$HERE/AospResRewrite.java")" "$(native_path "$CLASSES")"

rm -f "$OUT/lib/aosp-resources.jar"
( cd "$CLASSES" && "$JAR" --create --file "$OUT/lib/aosp-resources.jar" . )
echo "[aosp-res] $OUT/lib/aosp-resources.jar: $(find "$CLASSES" -name '*.class' | wc -l) classes"

echo "[aosp-res] the framework's resources at API 34"
mkdir -p "$WORK/fwres" "$OUT/bin/aosp"
grep -E '^(resources\.arsc|AndroidManifest\.xml|res/.+[^/])$' "$WORK/all.txt" > "$WORK/fwres.txt"
(cd "$WORK/fwres" && xargs -d '\n' unzip -qo "$ALL" < "$WORK/fwres.txt")
# The three default_wallpaper PNGs (7.4 MB, a third of the table) become a 1x1 black PNG each. Nothing of
# Win.apk reads them: its WallpaperManager is its own (no built-in wallpaper: getBuiltInDrawable is null)
# and no AOSP class that names the id is in its jars. But an app can still reach them by name
# (getIdentifier("default_wallpaper", "drawable", "android")) through this table. Deleting the files
# would make such a read throw, and dropping their entries from resources.arsc would move the ids after
# them; a small valid image of the same format keeps the table as it is and the read working.
for f in "$WORK"/fwres/res/drawable*/default_wallpaper.png; do
    [[ -f "$f" ]] || continue
    printf '%s' 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR42mNgYGAAAAAEAAHI6uv5AAAAAElFTkSuQmCC' |
        base64 -d > "$f"
done
rm -f "$OUT/bin/aosp/framework-res-34.apk"
"$JAR" --create --no-manifest --file "$OUT/bin/aosp/framework-res-34.apk" -C "$WORK/fwres" .
rm -rf "$WORK"
du -sh "$OUT/lib/aosp-resources.jar" "$OUT/bin/aosp/framework-res-34.apk" | sed 's/^/[aosp-res] /'
