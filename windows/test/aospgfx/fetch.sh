#!/usr/bin/env bash
# Fetches what windows/test/aospgfx and windows/scripts/build_aosp_gfx.sh need into <deps dir>,
# once (CI caches the folder):
#   android-all-14.jar   AOSP 14's framework classes (API 34), as Robolectric builds them
#   nativeruntime/       from Robolectric's nativeruntime-dist-compat: the native library for this
#                        platform (native/<os>/x86_64/), ICU's data (icu/icudt68l.dat), the
#                        Android fonts with their fonts.xml (fonts/) and the hyphenation
#                        patterns (hyphen-data/)
# Both from Maven Central (its Google mirror: repo.maven.apache.org rate-limits CI), checked against
# the SHA-256 below. Unpacked with the JDK's `jar`, so it needs only curl and a JDK ($JAVA_HOME).
#
#   windows/test/aospgfx/fetch.sh <deps dir> [linux|windows|jar]
#
# The test downloads it; build_aosp_gfx.sh stages it into the runtime; build_java.sh compiles the
# framework against android-all's graphics classes and asks for the jar alone ("jar": no native
# runtime).
set -euo pipefail

DEPS=${1:?usage: fetch.sh <deps dir> [linux|windows|jar]}
OS=${2:-}
if [[ -z "$OS" ]]; then
    case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) OS=windows ;; *) OS=linux ;; esac
fi
MAVEN=https://maven-central.storage-download.googleapis.com/maven2
ALL_VER=14-robolectric-10818077
ALL_SHA=6be2218c6a53fe3c57bc22ebdc723edcb7270a8a6f187545708aa5c0ed813977
DIST_VER=1.0.18
DIST_SHA=a6c34d92c990754e5aee076032a36f8e38fcacb5eb878ebe0a324509f3864eef

JAR="${JAVA_HOME:+$JAVA_HOME/bin/}jar"
mkdir -p "$DEPS"

fetch() {  # url file sha256
    if [[ -f "$2" ]] && echo "$3  $2" | sha256sum -c --status; then return 0; fi
    echo "fetching $1"
    curl -fL --retry 5 --retry-delay 5 --retry-all-errors -sS -o "$2.part" "$1"
    if ! echo "$3  $2.part" | sha256sum -c --status; then
        echo "SHA-256 mismatch for $1: got $(sha256sum "$2.part" | cut -d' ' -f1)" >&2
        exit 1
    fi
    mv "$2.part" "$2"
}

fetch "$MAVEN/org/robolectric/android-all/$ALL_VER/android-all-$ALL_VER.jar" "$DEPS/android-all-14.jar" "$ALL_SHA"
[[ "$OS" == jar ]] && exit 0

NR="$DEPS/nativeruntime"
case "$OS" in
    windows) LIB=native/windows/x86_64/robolectric-nativeruntime.dll ;;
    *) LIB=native/linux/x86_64/librobolectric-nativeruntime.so ;;
esac
if [[ ! -f "$NR/$LIB" || ! -f "$NR/icu/icudt68l.dat" || ! -f "$NR/fonts/fonts.xml" || ! -d "$NR/hyphen-data" ]]; then
    dist="$DEPS/nativeruntime-dist-compat-$DIST_VER.jar"
    fetch "$MAVEN/org/robolectric/nativeruntime-dist-compat/$DIST_VER/nativeruntime-dist-compat-$DIST_VER.jar" \
        "$dist" "$DIST_SHA"
    rm -rf "$NR" && mkdir -p "$NR"
    # `jar x` extracts into the current folder.
    abs_dist=$(cd "$(dirname "$dist")" && pwd)/$(basename "$dist")
    (cd "$NR" && "$JAR" xf "$abs_dist" "$LIB" icu fonts hyphen-data)
    rm -f "$dist"  # 159 MB; what the test and the stage need is unpacked
fi
ls -l "$DEPS/android-all-14.jar" "$NR/$LIB" "$NR/icu/icudt68l.dat"
echo "fonts: $(ls "$NR/fonts" | wc -l) files"
