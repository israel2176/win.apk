#!/usr/bin/env bash
# Builds the UI probe: <out>/probe.apk (for Win.apk) and <out>/classes (the same
# classes, for the Robolectric oracle).
#
#   windows/test/uiprobe/build.sh <out dir>
#
# Needs an Android SDK (ANDROID_HOME, or %LOCALAPPDATA%\Android\Sdk) with a
# build-tools and a platform, and a JDK (JAVA_HOME, 11 or later).
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
OUT="${1:?usage: build.sh <out dir>}"
mkdir -p "$OUT"
OUT="$(cd -- "$OUT" && pwd)"

SDK="${ANDROID_HOME:-${LOCALAPPDATA:-$HOME}/Android/Sdk}"
BT="$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)"
PLATFORM="$(ls -d "$SDK"/platforms/android-* | sort -V | tail -1)"
JAR="$PLATFORM/android.jar"
JAVAC="${JAVA_HOME:+$JAVA_HOME/bin/}javac"
JAVA="${JAVA_HOME:+$JAVA_HOME/bin/}java"
KEYTOOL="${JAVA_HOME:+$JAVA_HOME/bin/}keytool"
native() { if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }

rm -rf "$OUT/gen" "$OUT/classes" "$OUT/dex" "$OUT/in"
mkdir -p "$OUT/gen" "$OUT/classes" "$OUT/dex" "$OUT/in"
# The tools get a copy under <out>: aapt2 does not open a path outside the
# ANSI code page (a checkout under a folder with a Hebrew name).
cp -r "$HERE/res" "$HERE/src" "$HERE/AndroidManifest.xml" "$OUT/in/"
HERE="$OUT/in"

"$BT/aapt2" compile --dir "$(native "$HERE/res")" -o "$(native "$OUT/res.zip")"
"$BT/aapt2" link -o "$(native "$OUT/probe.unsigned.apk")" -I "$(native "$JAR")" \
    --manifest "$(native "$HERE/AndroidManifest.xml")" --java "$(native "$OUT/gen")" "$(native "$OUT/res.zip")"

find "$HERE/src" "$OUT/gen" -name '*.java' | while read -r f; do native "$f"; done > "$OUT/sources.list"
"$JAVAC" -nowarn -source 8 -target 8 -Xlint:-options -bootclasspath "$(native "$JAR")" \
    -d "$(native "$OUT/classes")" @"$(native "$OUT/sources.list")"

find "$OUT/classes" -name '*.class' | while read -r f; do native "$f"; done > "$OUT/classes.list"
"$JAVA" -cp "$(native "$BT/lib/d8.jar")" com.android.tools.r8.D8 --min-api 26 --lib "$(native "$JAR")" \
    --output "$(native "$OUT/dex")" @"$(native "$OUT/classes.list")"

cp -f "$OUT/probe.unsigned.apk" "$OUT/probe.unaligned.apk"
( cd "$OUT/dex" && "${JAVA_HOME:+$JAVA_HOME/bin/}jar" uf "$(native "$OUT/probe.unaligned.apk")" classes.dex )
"$BT/zipalign" -f 4 "$(native "$OUT/probe.unaligned.apk")" "$(native "$OUT/probe.apk")"

KS="$OUT/debug.keystore"
if [[ ! -f "$KS" ]]; then
    "$KEYTOOL" -genkeypair -keystore "$(native "$KS")" -storepass android -keypass android -alias probe \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Win.apk UI probe" > /dev/null 2>&1
fi
"$JAVA" -jar "$(native "$BT/lib/apksigner.jar")" sign --ks "$(native "$KS")" --ks-pass pass:android \
    --key-pass pass:android "$(native "$OUT/probe.apk")"
echo "[uiprobe] $OUT/probe.apk"
