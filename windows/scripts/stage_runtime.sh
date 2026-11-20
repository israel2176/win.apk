#!/usr/bin/env bash
# stage_runtime.sh -- assembles a runnable Win.apk runtime folder:
#
#   <out>/Win.apk.exe                 the app library (launcher/)
#   <out>/bin/winapk-runtime.exe
#   <out>/bin/macapk-translator.exe, winapk-guest-stub.dll (app native libraries)
#   <out>/bin/libEGL.dll, libGLESv2.dll, ... (ANGLE, for GL; fetch_angle.sh)
#   <out>/bin/WebView2Loader.dll      (WebView2, for web content; fetch_webview2.sh)
#   <out>/bin/tools/framework-res.apk
#   <out>/lib/*.jar, java_base_patch/, jdk_unsupported_patch/
#   <out>/lib/aosp-graphics.jar, bin/robolectric-nativeruntime.dll, bin/aosp/ (ICU's data, the
#                                     fonts): AOSP's graphics, what apps draw with unless
#                                     WINAPK_AOSP_GFX=0 (build_aosp_gfx.sh). WINAPK_STAGE_AOSP_GFX=0 leaves them
#                                     out; WINAPK_AOSP_GFX_DEPS names the download folder
#                                     (default thirdparty/aosp-gfx)
#   <out>/lib/aosp-resources.jar, bin/aosp/framework-res-34.apk: AOSP's own resources, only with
#                                     WINAPK_STAGE_AOSP_RES=1 (build_aosp_res.sh; the switch,
#                                     WINAPK_AOSP_RES=1, is new and off by default)
#   <out>/lib/aosp-views.jar:         AOSP's own views, only with WINAPK_STAGE_AOSP_RES=1 and
#                                     WINAPK_STAGE_AOSP_VIEWS=1 (build_aosp_views.sh; the switch,
#                                     WINAPK_AOSP_VIEWS=1, is new and off by default)
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
    if [[ "${WINAPK_STAGE_AOSP_RES:-0}" == 1 ]]; then
        "$HERE/build_aosp_res.sh" "$OUT" "${WINAPK_AOSP_GFX_DEPS:-}"
        if [[ "${WINAPK_STAGE_AOSP_VIEWS:-0}" == 1 ]]; then
            "$HERE/build_aosp_views.sh" "$OUT" "${WINAPK_AOSP_GFX_DEPS:-}"
        fi
    fi
fi
cp -f "$BUILD/winapk-runtime.exe" "$BUILD/macapk-translator.exe" "$BUILD/winapk-guest-stub.dll" "$OUT/bin/"
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
