#!/usr/bin/env bash
# run_ours.sh under Wine, on Linux: runs the probe APK in a staged runtime (a
# build's stage, or the runtime-stage artifact a CI run uploads) and writes
# <out>/ours.txt. With build.sh's oracle beside it, compare.py then lists what
# Win.apk does differently from Android without a Windows machine.
#
#   windows/test/uiprobe/run_ours_wine.sh <out dir of build.sh> <stage dir> [runtime args...]
#
# <stage dir> holds bin/winapk-runtime.exe, lib/ and jre/. Needs wine64 and
# mingw-w64 gcc (for wine_iphlpapi.sh); a display is not needed (headless).
# Wine 9 aborts at start ("free(): invalid pointer") when XDG_RUNTIME_DIR
# names no directory, as in a container with no /run/user/<uid>: one is made.
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
OUT="$(cd -- "${1:?usage: run_ours_wine.sh <out> <stage dir>}" && pwd)"
STAGE="$(cd -- "${2:?usage: run_ours_wine.sh <out> <stage dir>}" && pwd)"
shift 2
WINE="${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)}"
export WINE WINEDEBUG="${WINEDEBUG:--all}"
if [[ -z "${XDG_RUNTIME_DIR:-}" || ! -d "$XDG_RUNTIME_DIR" ]]; then
    export XDG_RUNTIME_DIR="$OUT/xdg-runtime"
    mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
fi

# The JDK's NetworkInterface needs a function Wine 9 lacks (see the script).
"$HERE/../wine_iphlpapi.sh" > /dev/null
winpath() { "$WINE" winepath -w "$1" 2>/dev/null; }
printf 'security.provider.1=SunMSCAPI\nsecurity.provider.2=SUN\n' > "$OUT/wine.security"

mkdir -p "$OUT/data"
# The oracle's screen: 360x640 dp at xhdpi, as run_ours.sh.
MACAPK_JVM_OPTS="-Djava.security.properties=$(winpath "$OUT/wine.security")" \
WINEDLLOVERRIDES="iphlpapi=n,b" \
WINAPK_HEADLESS=1 WINAPK_HEADLESS_EXIT_AFTER_FRAMES=90 WINAPK_PHONE_SIZE=360x640 WINAPK_DENSITY=320 \
WINAPK_SHORT_DATA_DIR=0 \
    "$WINE" "$STAGE/bin/winapk-runtime.exe" --apk "$(winpath "$OUT/probe.apk")" \
    --data "$(winpath "$OUT/data")" "$@" > "$OUT/ours.log" 2>&1 || true
grep '^PROBE ' "$OUT/ours.log" | sed 's/^PROBE //' | tr -d '\r' > "$OUT/ours.txt"
if ! grep -q '^done$' "$OUT/ours.txt"; then
    echo "[uiprobe] the probe did not finish in Win.apk; see $OUT/ours.log" >&2
    tail -20 "$OUT/ours.log" >&2
    exit 1
fi
echo "[uiprobe] $OUT/ours.txt ($(wc -l < "$OUT/ours.txt") lines)"
