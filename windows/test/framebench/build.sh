#!/usr/bin/env bash
# Builds the frame bench (windows/README.md, "Frame time"): a one-screen APK of plain framework
# views that scrolls a list by itself on every frame, so a run of it draws a frame every vsync for
# as long as it runs. No Gradle and no library: aapt2, javac, d8 and apksigner from an Android SDK.
#
#   windows/test/framebench/build.sh <out dir>    # <out>/framebench.apk
#
# Needs an Android SDK with build-tools and a platform (ANDROID_HOME or ANDROID_SDK_ROOT; the
# newest of each is taken), a JDK (JAVA_HOME or PATH: javac, jar, keytool, java). Signed with a
# key made for the build: Win.apk takes any signature, as Android takes a sideloaded app.
#
# Run it with WINAPK_HEADLESS_EXIT_AFTER_FRAMES=<vsyncs> (60 a second): at exit the runtime logs
# its `[win.apk] frame times` line (run.sh).
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
OUT="${1:?usage: build.sh <out dir>}"
mkdir -p "$OUT"
OUT="$(cd -- "$OUT" && pwd)"
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
if command -v cygpath > /dev/null 2>&1 && [[ -n "$SDK" ]]; then SDK="$(cygpath -u "$SDK")"; fi
[[ -n "$SDK" && -d "$SDK/build-tools" ]] || { echo "[framebench] no Android SDK (ANDROID_HOME)" >&2; exit 1; }
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

W="$OUT/.framebench"
rm -rf "$W"
mkdir -p "$W/classes" "$W/dex"
"$AAPT2" link -o "$(native "$W/base.apk")" -I "$(native "$ANDROID_JAR")" \
    --manifest "$(native "$HERE/AndroidManifest.xml")"
"${JBIN}javac" -source 8 -target 8 -Xlint:-options -encoding UTF-8 -bootclasspath "$(native "$ANDROID_JAR")" \
    -d "$(native "$W/classes")" "$(native "$HERE/src/dev/winapk/framebench/BenchActivity.java")"
# d8 and apksigner as jars: their .bat and shell wrappers differ between the SDK's hosts.
classes=()
while IFS= read -r f; do classes+=("$(native "$f")"); done < <(find "$W/classes" -name '*.class')
"${JBIN}java" -cp "$(native "$BT/lib/d8.jar")" com.android.tools.r8.D8 --release --min-api 21 \
    --lib "$(native "$ANDROID_JAR")" --output "$(native "$W/dex")" "${classes[@]}"
cp "$W/base.apk" "$W/unsigned.apk"
"${JBIN}jar" uf "$(native "$W/unsigned.apk")" -C "$(native "$W/dex")" classes.dex
"${JBIN}keytool" -genkeypair -keystore "$(native "$W/key.jks")" -storepass framebench -keypass framebench \
    -alias bench -keyalg RSA -keysize 2048 -validity 3650 -dname "CN=Win.apk frame bench" > /dev/null 2>&1
"${JBIN}java" -jar "$(native "$BT/lib/apksigner.jar")" sign --ks "$(native "$W/key.jks")" \
    --ks-pass pass:framebench --out "$(native "$OUT/framebench.apk")" "$(native "$W/unsigned.apk")"
echo "[framebench] $OUT/framebench.apk"
