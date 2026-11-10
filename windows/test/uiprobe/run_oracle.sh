#!/usr/bin/env bash
# Runs the probes on the real AOSP framework (Robolectric) and writes
# <out>/oracle.txt: what Android does.
#
#   windows/test/uiprobe/run_oracle.sh <out dir of build.sh> <dir of Robolectric's jars>
#
# The jars: `mvn dependency:copy-dependencies` of org.robolectric:robolectric
# and junit:junit (with Google's Maven repository added for androidx.test).
# Robolectric fetches the Android framework it runs (android-all) from Maven
# Central the first time.
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
OUT="$(cd -- "${1:?usage: run_oracle.sh <out> <libs>}" && pwd)"
LIBS="$(cd -- "${2:?usage: run_oracle.sh <out> <libs>}" && pwd)"
JAVAC="${JAVA_HOME:+$JAVA_HOME/bin/}javac"
JAVA="${JAVA_HOME:+$JAVA_HOME/bin/}java"
native() { if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }
SEP=":"
command -v cygpath > /dev/null 2>&1 && SEP=";"

# An .aar's code is its classes.jar.
for aar in "$LIBS"/*.aar; do
    [[ -f "$aar" ]] || continue
    jar="$LIBS/$(basename "$aar" .aar)-classes.jar"
    [[ -f "$jar" ]] || unzip -p "$aar" classes.jar > "$jar"
done
CP="$(native "$OUT/classes")"
for jar in "$LIBS"/*.jar; do CP="$CP$SEP$(native "$jar")"; done

# Where Robolectric finds the app: its manifest and its compiled resources.
CONF="$OUT/oracle-conf"
mkdir -p "$CONF/com/android/tools" "$OUT/oracle-classes"
cat > "$CONF/com/android/tools/test_config.properties" <<EOF
android_merged_manifest=$(native "$OUT/in/AndroidManifest.xml")
android_resource_apk=$(native "$OUT/probe.unsigned.apk")
android_custom_package=dev.winapk.probe
EOF
cp -f "$HERE/oracle/ProbeOracleTest.java" "$OUT/oracle-classes/"
# The SDK's android.jar, last on the class path as in a Gradle unit test: the
# test class and Robolectric's annotations name Android classes before the
# sandbox exists. Inside it the framework is Robolectric's own (android-all).
SDK="${ANDROID_HOME:-${LOCALAPPDATA:-$HOME}/Android/Sdk}"
PLATFORM="$(ls -d "$SDK"/platforms/android-* | sort -V | tail -1)"
CP="$CP$SEP$(native "$PLATFORM/android.jar")"
"$JAVAC" -nowarn -cp "$CP" -d "$(native "$OUT/oracle-classes")" "$(native "$OUT/oracle-classes/ProbeOracleTest.java")"

# user.home under <out>: Robolectric keeps the framework it fetched in ~/.m2
# and cannot load its native library from a home folder with a space in its
# name (it uses the URL's text, "%20" and all, as the path).
mkdir -p "$OUT/home"
"$JAVA" "-Duser.home=$(native "$OUT/home")" -cp "$(native "$OUT/oracle-classes")$SEP$(native "$CONF")$SEP$CP" \
    org.junit.runner.JUnitCore ProbeOracleTest > "$OUT/oracle.log" 2>&1 || true
grep '^PROBE ' "$OUT/oracle.log" | sed 's/^PROBE //' > "$OUT/oracle.txt"
if ! grep -q '^done$' "$OUT/oracle.txt"; then
    echo "[uiprobe] the oracle did not finish; see $OUT/oracle.log" >&2
    tail -20 "$OUT/oracle.log" >&2
    exit 1
fi
echo "[uiprobe] $OUT/oracle.txt ($(wc -l < "$OUT/oracle.txt") lines)"
