#!/usr/bin/env bash
# Every screen of an app against Android, without logging in: every layout
# resource of the APK inflated, measured, laid out and drawn at 360x640 dp, left
# to right and right to left, on the real framework (Robolectric) and in Win.apk
# (LayoutSweep, MACAPK_LAYOUT_SWEEP=1), then compared.
#
#   windows/test/screens/screens.sh <apk> <out dir> <winapk-runtime.exe> [options]
#
#   --theme <0x7f.. | style/Name>  the theme to inflate in (default: the one most
#                                  activities wear, manifest_info.py)
#   --filter <regex>               only layouts whose name matches
#   --pictures                     also save every case as a PNG on both sides
#   --skip-oracle / --skip-ours    reuse the side already in <out>
#
# Writes <out>/oracle.txt, <out>/ours.txt, <out>/diff.txt (compare.py's) and
# <out>/summary.txt (summarize.py's: layouts that throw only in Win.apk first,
# then the layouts with the most differing lines).
#
# Needs: an Android SDK with build-tools (aapt2) and platform 34 (ANDROID_HOME),
# a JDK 17+ (JAVA_HOME), Gradle 8.7+ (GRADLE, default gradle on PATH), Python 3.
# Robolectric fetches its Android framework into SCREENS_M2 (default ~/.m2).
# The runtime's lib/ beside its bin/ (a staged runtime) supplies the DEX
# converter. On Linux set WINE to wine64 to run Win.apk under Wine
# (WINEPREFIX as usual). <out> must be a path without spaces or non-ASCII
# characters (Robolectric fails on those).
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
REPO="$(cd -- "$HERE/../../.." && pwd)"
APK="${1:?usage: screens.sh <apk> <out> <winapk-runtime.exe> [--theme t] [--filter re] [--pictures]}"
OUT="${2:?usage: screens.sh <apk> <out> <winapk-runtime.exe>}"
RUNTIME="${3:?usage: screens.sh <apk> <out> <winapk-runtime.exe>}"
shift 3
THEME_ARG="" FILTER="" PICTURES=0 SKIP_ORACLE=0 SKIP_OURS=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --theme) THEME_ARG="$2"; shift 2 ;;
        --filter) FILTER="$2"; shift 2 ;;
        --pictures) PICTURES=1; shift ;;
        --skip-oracle) SKIP_ORACLE=1; shift ;;
        --skip-ours) SKIP_OURS=1; shift ;;
        *) echo "screens.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

mkdir -p "$OUT"
OUT="$(cd -- "$OUT" && pwd)"
APK="$(cd -- "$(dirname -- "$APK")" && pwd)/$(basename -- "$APK")"
RUNTIME="$(cd -- "$(dirname -- "$RUNTIME")" && pwd)/$(basename -- "$RUNTIME")"
STAGE_LIB="$(dirname -- "$(dirname -- "$RUNTIME")")/lib"
SDK="${ANDROID_HOME:-${LOCALAPPDATA:-$HOME}/Android/Sdk}"
GRADLE="${GRADLE:-gradle}"
JAVA="${JAVA_HOME:+$JAVA_HOME/bin/}java"
AAPT2="$(ls -d "$SDK"/build-tools/*/ | sort -V | tail -1)aapt2"
ANDROID_JAR="$SDK/platforms/android-34/android.jar"
native() { if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }
log() { echo "[screens] $*"; }

# ---- What the manifest says: the package, the theme, right to left ----
eval "$(python3 "$HERE/manifest_info.py" "$AAPT2" "$APK" "$OUT/AndroidManifest.xml")"
THEME="${THEME_ARG:-$THEME}"
log "$PACKAGE, theme $THEME${THEME_ARG:+ (given)}${THEME_ARG:- ($THEME_WHY)}, supportsRtl=$RTL"

# ---- Android: Robolectric over the APK's resources and its classes ----
if [[ $SKIP_ORACLE == 0 ]]; then
    if [[ ! -s "$OUT/classes.jar" || "$APK" -nt "$OUT/classes.jar" ]]; then
        # Win.apk's own converter: plain JVM classes with stack maps, which Robolectric's
        # instrumenting class loader takes as they are. Without its HostDefaults class,
        # which routes Unsafe, System.exit and System.load to the runtime's own classes
        # (com.macapk.loader.MacApk*): on Android the app's calls go where it wrote them.
        log "converting the DEX to $OUT/classes.jar"
        python3 - "$STAGE_LIB/macapk-loader.jar" "$OUT/dex2jvm.jar" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as src, zipfile.ZipFile(sys.argv[2], 'w', zipfile.ZIP_DEFLATED) as dst:
    for e in src.infolist():
        if e.filename.startswith('com/macapk/dex/') and e.filename != 'com/macapk/dex/HostDefaults.class':
            dst.writestr(e, src.read(e))
PY
        "$JAVA" -Xmx4g -cp "$(native "$OUT/dex2jvm.jar")" com.macapk.dex.Main \
            --classpath "$(native "$ANDROID_JAR")" "$(native "$APK")" "$(native "$OUT/classes.jar")" \
            > "$OUT/convert.log" 2>&1 || { tail -5 "$OUT/convert.log" >&2; exit 1; }
    fi
    P="$OUT/oracle"
    rm -rf "$P/oracle" "$P/sweep" "$P/config"
    mkdir -p "$P/sweep/com/macapk/loader" "$P/config/com/android/tools"
    cp "$HERE/build.gradle" "$HERE/settings.gradle" "$P/"
    cp -r "$HERE/oracle" "$P/"
    cp "$REPO/runtime/java/com/macapk/loader/LayoutSweep.java" "$P/sweep/com/macapk/loader/"
    # Where Robolectric finds an app's resources: what the Android Gradle plugin writes
    # for includeAndroidResources, here pointing at the APK itself.
    {
        echo "android_resource_apk=$(native "$APK")"
        echo "android_merged_manifest=$(native "$OUT/AndroidManifest.xml")"
        echo "android_custom_package=$PACKAGE"
    } > "$P/config/com/android/tools/test_config.properties"
    # Robolectric fetches the Android framework it runs into ~/.m2 (the test's user.home
    # is build/home): reuse one already fetched (SCREENS_M2, default ~/.m2).
    M2="${SCREENS_M2:-$HOME/.m2}"
    if [[ -d "$M2" ]]; then mkdir -p "$P/build/home"; ln -sfn "$M2" "$P/build/home/.m2"; fi
    rm -rf "$OUT/oracle-pictures"
    log "Android (Robolectric): $OUT/oracle.txt"
    ( cd "$P" && "$GRADLE" -q --no-daemon test \
        "-Pscreens.appJar=$(native "$OUT/classes.jar")" "-Pscreens.androidJar=$(native "$ANDROID_JAR")" \
        "-Pscreens.out=$(native "$OUT/oracle.txt")" "-Pscreens.theme=$THEME" "-Pscreens.filter=$FILTER" \
        "-Pscreens.pictures=$([[ $PICTURES == 1 ]] && native "$OUT/oracle-pictures")" ) \
        > "$OUT/oracle.log" 2>&1 || { tail -30 "$OUT/oracle.log" >&2; }
    grep -q '^done$' "$OUT/oracle.txt" 2>/dev/null || { echo "[screens] the oracle did not finish; see $OUT/oracle.log" >&2; exit 1; }
fi

# ---- Win.apk: the runtime with MACAPK_LAYOUT_SWEEP=1 ----
if [[ $SKIP_OURS == 0 ]]; then
    mkdir -p "$OUT/data"
    if [[ -n "${WINE:-}" ]]; then
        wpath() { "$WINE" winepath -w "$1" 2>/dev/null; }
        # Wine's workarounds (windows/test/run_app_wine.sh): its own iphlpapi, and
        # no security provider that asks Wine's crypto for what it lacks.
        printf 'security.provider.1=SunMSCAPI\nsecurity.provider.2=SUN\n' > "$OUT/wine.security"
        export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" WINEDEBUG="${WINEDEBUG:--all}"
        export WINEDLLOVERRIDES="iphlpapi=n,b" WINAPK_SHORT_DATA_DIR=0
        export MACAPK_JVM_OPTS="-Djava.security.properties=$(wpath "$OUT/wine.security")"
        run=("$WINE" "$RUNTIME")
    else
        wpath() { if command -v cygpath > /dev/null 2>&1; then cygpath -w "$1"; else printf '%s\n' "$1"; fi; }
        run=("$RUNTIME")
    fi
    rm -rf "$OUT/ours-pictures"
    log "Win.apk: $OUT/ours.txt"
    # The oracle's screen: 360x640 dp at xhdpi.
    MACAPK_LAYOUT_SWEEP=1 MACAPK_LAYOUT_SWEEP_THEME="$THEME" MACAPK_LAYOUT_SWEEP_FILTER="$FILTER" \
    MACAPK_LAYOUT_SWEEP_PICTURES="$([[ $PICTURES == 1 ]] && wpath "$OUT/ours-pictures")" \
    WINAPK_HEADLESS=1 WINAPK_PHONE_SIZE=360x640 WINAPK_DENSITY=320 \
        timeout "${SCREENS_TIMEOUT:-3600}" "${run[@]}" --apk "$(wpath "$APK")" --data "$(wpath "$OUT/data")" \
        > "$OUT/ours.log" 2>&1 || true
    grep '^PROBE ' "$OUT/ours.log" | sed 's/^PROBE //' | tr -d '\r' > "$OUT/ours.txt" || true
    grep -q '^done$' "$OUT/ours.txt" || { echo "[screens] Win.apk did not finish; see $OUT/ours.log" >&2; tail -20 "$OUT/ours.log" >&2; }
fi

# ---- The difference ----
python3 "$REPO/windows/test/uiprobe/compare.py" "$OUT/oracle.txt" "$OUT/ours.txt" > "$OUT/diff.txt" || true
python3 "$HERE/summarize.py" "$OUT/oracle.txt" "$OUT/ours.txt" > "$OUT/summary.txt"
log "$OUT/diff.txt: $(tail -1 "$OUT/diff.txt")"
log "$OUT/summary.txt"
head -40 "$OUT/summary.txt"
