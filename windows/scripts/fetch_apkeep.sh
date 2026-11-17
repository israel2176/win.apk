#!/usr/bin/env bash
# fetch_apkeep.sh -- apkeep (EFF, MIT), the command-line downloader the
# Google services setup's Download runs: it fetches Google's three apps from
# APKPure, x86_64 variant, into the setup's own folder, and the launcher
# installs a file only once `--verify-apk` says Google signed it.
#
#   windows/scripts/fetch_apkeep.sh [bin dir]
#
# The Windows build of a pinned release, from the project's GitHub releases,
# kept in thirdparty/apkeep/ (not in git) so a rebuild does not download it
# again. With a bin dir it is copied to <bin>/tools/apkeep.exe and its licence
# to <bin>/licenses/apkeep-LICENSE.txt.
#
# SHA256 pins the file. Empty, the download is taken as it is and its hash
# printed: copy it here, so the next release checks it.

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
VERSION=1.1.0
SHA256=
DIR="$HERE/../../thirdparty/apkeep"
EXE="$DIR/apkeep-$VERSION.exe"
ASSET=apkeep-x86_64-pc-windows-msvc.exe

mkdir -p "$DIR"
check() { [[ -z "$SHA256" ]] || echo "$SHA256  $1" | sha256sum -c --status; }
if [[ ! -f "$EXE" ]] || ! check "$EXE"; then
    echo "[apkeep] downloading apkeep $VERSION"
    ok=0
    # The project has tagged releases both with and without a leading v.
    for tag in "$VERSION" "v$VERSION"; do
        if curl -sSfL -o "$EXE.part" "https://github.com/EFForg/apkeep/releases/download/$tag/$ASSET"; then
            ok=1
            break
        fi
    done
    [[ $ok == 1 ]] || { echo "[apkeep] no $ASSET in release $VERSION" >&2; rm -f "$EXE.part"; exit 1; }
    check "$EXE.part" || { echo "[apkeep] checksum mismatch" >&2; rm -f "$EXE.part"; exit 1; }
    [[ -n "$SHA256" ]] || echo "[apkeep] not pinned yet: SHA256=$(sha256sum "$EXE.part" | cut -d' ' -f1)"
    mv "$EXE.part" "$EXE"
fi
if [[ ! -f "$DIR/LICENSE" ]]; then
    curl -sSfL -o "$DIR/LICENSE" "https://raw.githubusercontent.com/EFForg/apkeep/$VERSION/LICENSE" \
        || curl -sSfL -o "$DIR/LICENSE" "https://raw.githubusercontent.com/EFForg/apkeep/master/LICENSE"
fi
if [[ -n "${1:-}" ]]; then
    mkdir -p "$1/tools" "$1/licenses"
    cp -f "$EXE" "$1/tools/apkeep.exe"
    cp -f "$DIR/LICENSE" "$1/licenses/apkeep-LICENSE.txt"
    echo "[apkeep] apkeep.exe ($VERSION) in $1/tools"
fi
