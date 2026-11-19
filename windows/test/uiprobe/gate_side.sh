#!/usr/bin/env bash
# One side of the CI's UI gate (ci.yml, "UI gate"): Win.apk's lines for one
# target on one staged runtime, from the oracle job's files.
#
#   windows/test/uiprobe/gate_side.sh probe   <oracle dir> <stage dir> <out dir>
#   windows/test/uiprobe/gate_side.sh screens <oracle dir> <stage dir> <out dir>
#
# probe:   <oracle dir> holds build.sh's probe.apk and oracle.txt; run_ours.sh.
# screens: <oracle dir> holds app.apk and screens.sh's oracle.txt, manifest.env
#          and AndroidManifest.xml; screens.sh --skip-oracle (no Android SDK).
# Writes <out dir>/ours.txt. A run that stops short (no "done", or under 90%
# of Android's cases: gate.py complete) is run once more on a fresh data
# folder, as a first run sometimes is. Exit 1 when the second is short too.
set -uo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
KIND="${1:?usage: gate_side.sh probe|screens <oracle dir> <stage dir> <out dir>}"
ORACLE="$(cd -- "${2:?oracle dir}" && pwd)"
STAGE="$(cd -- "${3:?stage dir}" && pwd)"
OUT="${4:?out dir}"
PY="${PYTHON:-python3}"
mkdir -p "$OUT"
OUT="$(cd -- "$OUT" && pwd)"

for try in 1 2; do
    rm -rf "$OUT/data" "$OUT/ours.txt"
    case "$KIND" in
        probe)
            cp -f "$ORACLE/probe.apk" "$ORACLE/oracle.txt" "$OUT/"
            bash "$HERE/run_ours.sh" "$OUT" "$STAGE/bin/winapk-runtime.exe"
            ;;
        screens)
            cp -f "$ORACLE/oracle.txt" "$ORACLE/manifest.env" "$ORACLE/AndroidManifest.xml" "$OUT/"
            bash "$HERE/../screens/screens.sh" "$ORACLE/app.apk" "$OUT" "$STAGE/bin/winapk-runtime.exe" --skip-oracle
            ;;
        *) echo "gate_side.sh: unknown kind $KIND" >&2; exit 2 ;;
    esac
    if "$PY" "$HERE/gate.py" complete "$OUT/oracle.txt" "$OUT/ours.txt"; then
        exit 0
    fi
    [[ $try == 1 ]] && echo "[gate] short run; once more" >&2 && mv -f "$OUT/ours.log" "$OUT/ours-try1.log" 2>/dev/null
done
exit 1
