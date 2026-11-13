#!/usr/bin/env bash
# fetch_apkpure.sh [list] [out dir] -- fetches the apps of popular.txt from APKPure with apkeep
# (https://github.com/EFForg/apkeep, `cargo install apkeep`), for native_gaps.py and ApiCheck.
# The x86_64 build where APKPure has one, else arm64-v8a; a file is <package>@<abi>.<apk|xapk>.
#
# For analysis only: APKPure is not the apps' publisher (apkeep's USAGE-apkpure.md), so nothing
# fetched here is installed for use, redistributed or committed. APKPure serves a 32-bit split for
# some apps whatever is asked for, and does not carry every app; both are reported, not errors.
set -euo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
LIST="${1:-$HERE/popular.txt}"
OUT="${2:-$HERE/../../build/popular-apks}"
APKEEP="${APKEEP:-$(command -v apkeep || echo "$HOME/.cargo/bin/apkeep")}"
mkdir -p "$OUT"

has() { unzip -l "$1" 2>/dev/null | grep -q "$2"; }
fetch() {  # <package> <apkeep arch> -> the file's path, or nothing
    local t; t="$(mktemp -d)"
    timeout 900 "$APKEEP" -a "$1" -d apk-pure -o "acknowledge_dangers=true,arch=$2" "$t" >/dev/null 2>&1 || true
    ls "$t"/* 2>/dev/null | head -1
}
grep -v '^#' "$LIST" | while read -r pkg; do
    [[ -z "$pkg" ]] && continue
    ls "$OUT/$pkg"@* >/dev/null 2>&1 && continue
    f="$(fetch "$pkg" x86_64)"
    if [[ -n "$f" ]] && ! has "$f" 'lib/x86_64/\|config.x86_64.apk'; then rm -f "$f"; f=""; fi
    abi=x86_64
    if [[ -z "$f" ]]; then f="$(fetch "$pkg" arm64-v8a)"; abi=arm64; fi
    if [[ -z "$f" ]]; then echo "$pkg: not on APKPure"; continue; fi
    mv "$f" "$OUT/$pkg@$abi.${f##*.}"
    echo "$pkg: $abi"
done
