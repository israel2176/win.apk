#!/usr/bin/env bash
# Builds the surfaces test: one screen of what draws outside a view's own canvas -- a TextureView
# fed by lockCanvas, a SurfaceView on top (setZOrderOnTop) and one under the window, a HARDWARE
# bitmap, and a PixelCopy of the window -- each logging a `SURFACES` line of what it got. Built
# with the SDK's tools alone, as the frame bench.
#
#   windows/test/surfaces/build.sh <out dir>    # <out>/surfaces.apk
#
# Needs an Android SDK with build-tools and a platform (ANDROID_HOME or ANDROID_SDK_ROOT), a JDK.
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
OUT="${1:?usage: build.sh <out dir>}"
mkdir -p "$OUT"
OUT="$(cd -- "$OUT" && pwd)"
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
if command -v cygpath > /dev/null 2>&1 && [[ -n "$SDK" ]]; then SDK="$(cygpath -u "$SDK")"; fi
[[ -n "$SDK" && -d "$SDK/build-tools" ]] || { echo "[surfaces] no Android SDK (ANDROID_HOME)" >&2; exit 1; }
BT="$(ls -d "$SDK"/build-tools/*/ | sort -V | tail -1)"
PLATFORM="$(ls -d "$SDK"/platforms/android-*/ | sort -V | tail -1)"
ANDROID_JAR="$PLATFORM/android.jar"
JBIN=""
if [[ -n "${JAVA_HOME:-}" ]]; then
    JBIN="$JAVA_HOME/bin/"
    if command -v cygpath > /dev/null 2>&1; then JBIN="$(cygpath -u "$JAVA_HOME")/bin/"; fi
fi
# On Windows (MSYS2) the SDK's tools are .exe / .bat, and Java wants Windows paths.
native() { if command -v cygpath > /dev/null 2>&1; then cygpath -w "$1"; else printf '%s\n' "$1"; fi; }
AAPT2="$BT/aapt2"
[[ -f "$AAPT2" ]] || AAPT2="$BT/aapt2.exe"

W="$OUT/.surfaces"
rm -rf "$W"
mkdir -p "$W/classes" "$W/dex"
"$AAPT2" link -o "$(native "$W/base.apk")" -I "$(native "$ANDROID_JAR")" \
    --manifest "$(native "$HERE/AndroidManifest.xml")"
"${JBIN}javac" -source 8 -target 8 -Xlint:-options -encoding UTF-8 -bootclasspath "$(native "$ANDROID_JAR")" \
    -d "$(native "$W/classes")" "$(native "$HERE/src/dev/winapk/surfaces/SurfacesActivity.java")"
# d8 and apksigner as jars: their .bat and shell wrappers differ between the SDK's hosts.
classes=()
while IFS= read -r f; do classes+=("$(native "$f")"); done < <(find "$W/classes" -name '*.class')
"${JBIN}java" -cp "$(native "$BT/lib/d8.jar")" com.android.tools.r8.D8 --release --min-api 21 \
    --lib "$(native "$ANDROID_JAR")" --output "$(native "$W/dex")" "${classes[@]}"
cp "$W/base.apk" "$W/unsigned.apk"
"${JBIN}jar" uf "$(native "$W/unsigned.apk")" -C "$(native "$W/dex")" classes.dex
"${JBIN}keytool" -genkeypair -keystore "$(native "$W/key.jks")" -storepass surfaces -keypass surfaces \
    -alias bench -keyalg RSA -keysize 2048 -validity 3650 -dname "CN=Win.apk surfaces test" > /dev/null 2>&1
"${JBIN}java" -jar "$(native "$BT/lib/apksigner.jar")" sign --ks "$(native "$W/key.jks")" \
    --ks-pass pass:surfaces --out "$(native "$OUT/surfaces.apk")" "$(native "$W/unsigned.apk")"
echo "[surfaces] $OUT/surfaces.apk"
