#!/usr/bin/env bash
# Android's side of the CI's UI gate (ci.yml, "UI gate"), on Linux: the probe
# and every app of windows/test/screens/gate.txt on the real framework.
#
#   windows/test/uiprobe/gate_oracle.sh <out dir> <lib dir>
#
# <lib dir> is a build_java.sh lib/: its macapk-loader.jar converts the apps'
# DEX for Robolectric (screens.sh). Writes
#   <out>/probe/probe.apk, oracle.txt                  (build.sh)
#   <out>/<package>/app.apk, oracle.txt, manifest.env, AndroidManifest.xml
#                                                      (screens.sh --skip-ours)
#   <out>/targets.json   the gate's matrix: [{"name", "kind": probe|screens, "dir"}]
# and nothing else, so that the whole of <out> can be cached. Needs what
# build.sh and screens.sh need (an Android SDK, a JDK, Gradle as GRADLE).
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
REPO="$(cd -- "$HERE/../../.." && pwd)"
OUT="${1:?usage: gate_oracle.sh <out dir> <lib dir>}"
LIB="$(cd -- "${2:?usage: gate_oracle.sh <out dir> <lib dir>}" && pwd)"
PY="${PYTHON:-python3}"
mkdir -p "$OUT"
OUT="$(cd -- "$OUT" && pwd)"
WORK="$OUT/.work"
rm -rf "$WORK"

echo "[gate] the UI probe"
bash "$HERE/build.sh" "$WORK/probe" "${GRADLE:-gradle}"
mkdir -p "$OUT/probe"
cp -f "$WORK/probe/probe.apk" "$WORK/probe/oracle.txt" "$OUT/probe/"
targets='{"name": "UI probe", "kind": "probe", "dir": "probe"}'

# From descriptor 3: Gradle and the JVM must not read the rest of the list from stdin.
while read -r -u 3 pkg code sha; do
    [[ -z "$pkg" || "$pkg" == \#* ]] && continue
    echo "[gate] $pkg $code"
    d="$OUT/$pkg"
    mkdir -p "$d"
    "$PY" "$REPO/.github/probe/fdroid_fetch.py" "$pkg" "$code" "$d/app.apk" "$sha"
    SCREENS_LIB="$LIB" bash "$REPO/windows/test/screens/screens.sh" "$d/app.apk" "$WORK/$pkg" - --skip-ours
    cp -f "$WORK/$pkg/oracle.txt" "$WORK/$pkg/manifest.env" "$WORK/$pkg/AndroidManifest.xml" "$d/"
    targets="$targets, {\"name\": \"$pkg\", \"kind\": \"screens\", \"dir\": \"$pkg\"}"
done 3< "$REPO/windows/test/screens/gate.txt"

echo "[$targets]" > "$OUT/targets.json"
"$PY" -c 'import json, sys; json.load(open(sys.argv[1]))' "$OUT/targets.json"
rm -rf "$WORK"
echo "[gate] $OUT: $(cat "$OUT/targets.json")"
