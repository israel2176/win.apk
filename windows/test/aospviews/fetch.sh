#!/usr/bin/env bash
# Fetches what windows/test/aospviews needs into <deps dir>, once:
#   android-all-14.jar, nativeruntime/  through ../aospgfx/fetch.sh (AOSP 14's framework classes and
#                                       Robolectric's native runtime: Skia, minikin, HarfBuzz, ICU, fonts)
#   jars/                               Robolectric 4.16's Java port of libandroidfw (`resources`:
#                                       CppAssetManager2, LoadedArsc, ResXMLTree, AttributeResolution),
#                                       the shadows whose static "natives" the bridge calls
#                                       (`shadows-framework`) and the small jars those two load, and
#                                       ASM 9.8 for the build-time rewrite of AOSP's natives
# All from Maven Central's Google mirror, checked against the SHA-256 below.
#
#   windows/test/aospviews/fetch.sh <deps dir> [linux|windows]
set -euo pipefail

DEPS=${1:?usage: fetch.sh <deps dir> [linux|windows]}
HERE=$(cd "$(dirname "$0")" && pwd)
"$HERE/../aospgfx/fetch.sh" "$DEPS" "${2:-}"

MAVEN=https://maven-central.storage-download.googleapis.com/maven2
mkdir -p "$DEPS/jars"
fetch() {  # group-path artifact version sha256
    local f="$DEPS/jars/$2-$3.jar"
    if [[ -f "$f" ]] && echo "$4  $f" | sha256sum -c --status; then return 0; fi
    echo "fetching $2-$3"
    curl -fL --retry 5 --retry-delay 5 --retry-all-errors -sS -o "$f.part" "$MAVEN/$1/$2/$3/$2-$3.jar"
    if ! echo "$4  $f.part" | sha256sum -c --status; then
        echo "SHA-256 mismatch for $2-$3: got $(sha256sum "$f.part" | cut -d' ' -f1)" >&2
        exit 1
    fi
    mv "$f.part" "$f"
}
R=org/robolectric
fetch $R resources 4.16 0d6abfd274d3c853a564ea23ce1c52d31d075281ae0e7d64bbbc1477b2ed7679
fetch $R shadows-framework 4.16 f3ddb731c95cb4663634355bed62932ec8f2c7a3daab876885a16b48e1530eda
fetch $R utils 4.16 51940c4668ed065101c8a932c6b8a7efa7b546fd5a8e98180f40ac3840b27428
fetch $R utils-reflector 4.16 efa21dcd2d68b8f3e202580a141b5f338a33e194a46bcd33b70ddb9e6d5ad63d
fetch $R shadowapi 4.16 8502d044ab781668fc52eb8784dcacd50c54eaa819cf5cc5fbb1b8457c9931a6
fetch $R annotations 4.16 409f50354fc08b382a8fa8c42416438d3664d2cb4e63bf252022c875288fbf7a
fetch $R pluginapi 4.16 ffdb38f7d0a667e10d0609472ec13792018a69ccaf52c776166e03c61eae6428
# The port reads zip headers with Guava's Ints (FileMap); a few calls, but on the class path.
fetch com/google/guava guava 33.4.8-jre f3d7f57f67fd622f4d468dfdd692b3a5e3909246c28017ac3263405f0fe617ed
fetch org/ow2/asm asm 9.8 876eab6a83daecad5ca67eb9fcabb063c97b5aeb8cf1fca7a989ecde17522051
ls -l "$DEPS/jars"
