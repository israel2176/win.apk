#!/usr/bin/env bash
# stage_runtime.sh -- assembles a runnable Win.apk runtime folder:
#
#   <out>/Win.apk.exe                 the app library (launcher/)
#   <out>/bin/winapk-runtime.exe
#   <out>/bin/macapk-translator.exe, winapk-guest-stub.dll (app native libraries)
#   <out>/bin/winapk-shell.dll       (Explorer's thumbnails of the app files)
#   <out>/bin/libEGL.dll, libGLESv2.dll, ... (ANGLE, for GL; fetch_angle.sh)
#   <out>/bin/WebView2Loader.dll      (WebView2, for web content; fetch_webview2.sh)
#   <out>/bin/tools/apkeep.exe        (Google services setup's downloads; fetch_apkeep.sh)
#   <out>/lib/*.jar, java_base_patch/, jdk_unsupported_patch/
#   <out>/lib/aosp-graphics.jar, bin/robolectric-nativeruntime.dll, bin/aosp/ (ICU's data, the
#                                     fonts): AOSP's graphics, the only graphics the framework has
#                                     (build_aosp_gfx.sh; without them nothing starts).
#                                     WINAPK_AOSP_GFX_DEPS names the download folder (default
#                                     thirdparty/aosp-gfx), which build_java.sh reads too
#   <out>/lib/aosp-resources.jar, bin/aosp/framework-res-34.apk, lib/aosp-views.jar: AOSP's own
#                                     resources and views, the only ones apps run on, staged with the
#                                     graphics (build_aosp_res.sh, build_aosp_views.sh), and
#                                     lib/aosp-app.jar, AOSP's app layer (build_aosp_app.sh). The classes
#                                     of lib/android-framework.jar they shadow are then dropped from
#                                     it (never loaded: the AOSP jars come first on the class path);
#                                     the jar as compiled stays in <cmake build dir>/java/ for the
#                                     tests that run Win.apk's framework (on a plain JVM, or in
#                                     winapk-runtime --java-test for those that draw)
#
#   windows/scripts/stage_runtime.sh <cmake build dir> [out]
#
# The JDK is not copied: the runtime uses <out>/jre if present, otherwise
# WINAPK_JAVA_HOME or JAVA_HOME.

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
BUILD="${1:?cmake build dir}"
OUT="${2:-$HERE/../build/runtime}"
mkdir -p "$OUT"
OUT="$(cd -- "$OUT" && pwd)"

[[ -f "$BUILD/winapk-runtime.exe" ]] || { echo "[stage] no winapk-runtime.exe in $BUILD" >&2; exit 1; }
"$HERE/build_java.sh" "$OUT"
"$HERE/build_aosp_gfx.sh" "$OUT" "${WINAPK_AOSP_GFX_DEPS:-}"
"$HERE/build_aosp_res.sh" "$OUT" "${WINAPK_AOSP_GFX_DEPS:-}"
"$HERE/build_aosp_views.sh" "$OUT" "${WINAPK_AOSP_GFX_DEPS:-}"
"$HERE/build_aosp_app.sh" "$OUT" "${WINAPK_AOSP_GFX_DEPS:-}"
# The framework as compiled, for the tests that run Win.apk's own views and resources (ci.yml,
# "Runtime tests"): on a plain JVM, or in winapk-runtime --java-test, which binds AOSP's graphics.
mkdir -p "$BUILD/java"
cp -f "$OUT/lib/android-framework.jar" "$BUILD/java/android-framework.jar"
# What aosp-resources.jar and aosp-views.jar shadow is never loaded (host/aosp_gfx.h puts them
# first): Win.apk's own android.content.res, android.view and android.widget, about 800 classes.
# Dropped from the shipped jar; LinkScan.java finds no reference that links less without them.
# The list goes into the jar, and build_aosp_*.sh refuse such a jar (their closures are
# computed against the whole framework).
DROP="$OUT/.build/aosp-shadowed.txt"
mkdir -p "$OUT/.build/drop/META-INF"
# Both listings sorted the same way (LC_ALL=C) and without carriage returns, for comm.
for j in aosp-resources aosp-views aosp-app; do unzip -Z1 "$OUT/lib/$j.jar"; done | tr -d '\r' | grep '\.class$' |
    LC_ALL=C sort -u > "$OUT/.build/aosp-shadowing.txt"
unzip -Z1 "$OUT/lib/android-framework.jar" | tr -d '\r' | LC_ALL=C sort > "$OUT/.build/framework-entries.txt"
# And Win.apk's own nested classes of a class AOSP's replaces (RemoteViews$1..$22, the anonymous
# classes of Win.apk's RemoteViews): never loaded through their outer class, which is AOSP's, but a
# name an AOSP class lacks still loads from here, beside AOSP's of the same outer class; reflection
# over them threw IncompatibleClassChangeError (Win.apk's RemoteViews$7 implementing an Action that
# AOSP's declares a class; ReflectScan.java).
LC_ALL=C comm -12 "$OUT/.build/aosp-shadowing.txt" "$OUT/.build/framework-entries.txt" > "$DROP.exact"
LC_ALL=C comm -23 "$OUT/.build/framework-entries.txt" "$OUT/.build/aosp-shadowing.txt" | grep '[$].*[.]class$' |
    awk -v shadowing="$OUT/.build/aosp-shadowing.txt" 'BEGIN { while ((getline l < shadowing) > 0) h[l] = 1 }
        { o = $0; sub(/[$].*/, "", o); if (h[o ".class"]) print }' > "$DROP.nested" || true
LC_ALL=C sort -u "$DROP.exact" "$DROP.nested" > "$DROP"
echo "[stage] android-framework.jar: $(wc -l < "$DROP") classes AOSP's resources, views and app layer shadow, dropped"
if [[ ! -s "$DROP" ]]; then
    echo "[stage] none found: the AOSP jars ($(wc -l < "$OUT/.build/aosp-shadowing.txt") classes) and the framework" \
         "($(wc -l < "$OUT/.build/framework-entries.txt") entries) share no name; first of each:" >&2
    head -n 3 "$OUT/.build/aosp-shadowing.txt" "$OUT/.build/framework-entries.txt" >&2
    exit 1
fi
zip -q -nw -d "$OUT/lib/android-framework.jar" -@ < "$DROP"
cp "$DROP" "$OUT/.build/drop/META-INF/winapk-aosp-shadowed.txt"
( cd "$OUT/.build/drop" && zip -q -X "$OUT/lib/android-framework.jar" META-INF/winapk-aosp-shadowed.txt )
echo "[stage] $(wc -l < "$DROP") classes of android-framework.jar that AOSP's resources and views shadow, dropped"
cp -f "$BUILD/winapk-runtime.exe" "$BUILD/macapk-translator.exe" "$BUILD/winapk-guest-stub.dll" "$OUT/bin/"
# Explorer's thumbnails of the app files (shell/apk_thumbnail.cpp), registered by the launcher and the installer.
cp -f "$BUILD/winapk-shell.dll" "$OUT/bin/"
# The arm64 translator, and the clang it compiles with (clang\, beside bin\),
# so no user has to install one.
if [[ -f "$BUILD/winapk-aot.exe" ]]; then
    cp -f "$BUILD/winapk-aot.exe" "$OUT/bin/"
    "$HERE/stage_clang.sh" "$OUT"
fi
cp -f "$BUILD/Win.apk.exe" "$OUT/"
# The tile an app without an icon gets in the library.
cp -f "$HERE/../launcher/winapk.png" "$OUT/bin/"
"$HERE/fetch_angle.sh" "$OUT/bin"
"$HERE/fetch_webview2.sh" "$OUT/bin"
"$HERE/fetch_apkeep.sh" "$OUT/bin"
rm -rf "$OUT/.build"  # build_java.sh's scratch, not part of the runtime
echo "[stage] runtime staged in $OUT"
