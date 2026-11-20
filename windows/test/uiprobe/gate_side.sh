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
#
# Each run is ended (run_until.py) after GATE_TIMEOUT seconds (default 480), or
# once GATE_STALL seconds (default 240) pass with no new PROBE line: a sweep
# takes a minute or less here (the DEX conversion ~10 s of it), so two tries
# on each side, base and head at once, stay under the job's 25 minutes.
#
# Exit 3, having run nothing, when the runtime cannot run this kind: screens on
# a runtime from before LayoutSweep (its macapk-loader.jar has no
# com/macapk/loader/LayoutSweep.class), which ignores MACAPK_LAYOUT_SWEEP and
# runs the app itself, a run that never ends. ci.yml then has no base for it.
set -uo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
KIND="${1:?usage: gate_side.sh probe|screens <oracle dir> <stage dir> <out dir>}"
ORACLE="$(cd -- "${2:?oracle dir}" && pwd)"
STAGE="$(cd -- "${3:?stage dir}" && pwd)"
OUT="${4:?out dir}"
PY="${PYTHON:-python3}"
mkdir -p "$OUT"
OUT="$(cd -- "$OUT" && pwd)"
export UIPROBE_TIMEOUT="${GATE_TIMEOUT:-480}" SCREENS_TIMEOUT="${GATE_TIMEOUT:-480}"
export RUN_UNTIL_STALL="${GATE_STALL:-240}"
native() { if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }

if [[ $KIND == screens ]] && ! "$PY" - "$(native "$STAGE/lib/macapk-loader.jar")" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    sys.exit(0 if 'com/macapk/loader/LayoutSweep.class' in z.namelist() else 1)
PY
then
    echo "[gate] $STAGE/lib/macapk-loader.jar has no LayoutSweep: this runtime cannot sweep an app's screens" >&2
    exit 3
fi

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
