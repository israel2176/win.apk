#!/usr/bin/env bash
# stage_runtime.sh -- assembles a runnable Win.apk runtime folder:
#
#   <out>/Win.apk.exe                 the app library (launcher/)
#   <out>/bin/winapk-runtime.exe
#   <out>/bin/macapk-translator.exe, winapk-guest-stub.dll (app native libraries)
#   <out>/bin/winapk-shell.dll       (Explorer's thumbnails of the app files)
#   <out>/bin/libEGL.dll, libGLESv2.dll, ... (ANGLE, for GL; fetch_angle.sh)
#   <out>/bin/WebView2Loader.dll      (WebView2, for web content; fetch_webview2.sh)
#   <out>/bin/tools/framework-res.apk
#   <out>/lib/*.jar, java_base_patch/, jdk_unsupported_patch/
#   <out>/lib/aosp-graphics.jar, bin/robolectric-nativeruntime.dll, bin/aosp/ (ICU's data, the
#                                     fonts): AOSP's graphics, what apps draw with unless
#                                     WINAPK_AOSP_GFX=0 (build_aosp_gfx.sh). WINAPK_STAGE_AOSP_GFX=0 leaves them
#                                     out; WINAPK_AOSP_GFX_DEPS names the download folder
#                                     (default thirdparty/aosp-gfx)
#   <out>/lib/aosp-resources.jar, bin/aosp/framework-res-34.apk, lib/aosp-views.jar: AOSP's own
#                                     resources and views, the only ones apps run on, staged with the
#                                     graphics (build_aosp_res.sh, build_aosp_views.sh). The classes
#                                     of lib/android-framework.jar they shadow are then dropped from
#                                     it (never loaded: the AOSP jars come first on the class path);
#                                     the jar as compiled stays in <cmake build dir>/java/ for the
#                                     tests that run Win.apk's framework on a plain JVM
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
"$HERE/build_framework_res.sh" "$OUT/bin/tools/framework-res.apk"
if [[ "${WINAPK_STAGE_AOSP_GFX:-1}" != 0 ]]; then
    "$HERE/build_aosp_gfx.sh" "$OUT" "${WINAPK_AOSP_GFX_DEPS:-}"
    "$HERE/build_aosp_res.sh" "$OUT" "${WINAPK_AOSP_GFX_DEPS:-}"
    "$HERE/build_aosp_views.sh" "$OUT" "${WINAPK_AOSP_GFX_DEPS:-}"
    # The framework as compiled, for the tests that run it on a plain JVM (ci.yml, "Runtime tests"):
    # they have no natives for AOSP's classes, so they still run Win.apk's own of the same names.
    mkdir -p "$BUILD/java"
    cp -f "$OUT/lib/android-framework.jar" "$BUILD/java/android-framework.jar"
    # What aosp-resources.jar and aosp-views.jar shadow is never loaded (host/aosp_gfx.h puts them
    # first): Win.apk's own android.content.res, android.view and android.widget, about 800 classes.
    # Dropped from the shipped jar; LinkScan.java finds no reference that links less without them.
    # The list goes into the jar, and build_aosp_*.sh refuse such a jar (their closures are
    # computed against the whole framework).
    DROP="$OUT/.build/aosp-shadowed.txt"
    mkdir -p "$OUT/.build/drop/META-INF"
    for j in aosp-resources aosp-views; do unzip -Z1 "$OUT/lib/$j.jar"; done | grep '\.class$' | sort -u |
        comm -12 - <(unzip -Z1 "$OUT/lib/android-framework.jar" | sort) > "$DROP"
    zip -q -nw -d "$OUT/lib/android-framework.jar" -@ < "$DROP"
    cp "$DROP" "$OUT/.build/drop/META-INF/winapk-aosp-shadowed.txt"
    ( cd "$OUT/.build/drop" && zip -q -X "$OUT/lib/android-framework.jar" META-INF/winapk-aosp-shadowed.txt )
    echo "[stage] $(wc -l < "$DROP") classes of android-framework.jar that AOSP's resources and views shadow, dropped"
fi
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
rm -rf "$OUT/.build"  # build_java.sh's scratch, not part of the runtime
echo "[stage] runtime staged in $OUT"
