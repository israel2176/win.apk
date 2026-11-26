#!/usr/bin/env bash
# run_scan.sh -- fetch what the API gap survey reads and run its scanners (docs/research/api-gaps.md).
#
#   windows/scripts/api_gaps/run_scan.sh <work dir> <runtime-stage dir> [<apk>...]
#
# <runtime-stage dir> is CI's runtime-stage artifact unpacked (main's newest green ci.yml run): the jars
# that ship, after stage_runtime.sh dropped the framework classes AOSP's jars replace. The SDK's
# android.jar of levels 34, 35 and 36 (with 36's api-versions.xml) comes from dl.google.com and
# android-all 14 (AOSP's real bodies, the stub test's reference) from Maven Central, cached in <work dir>.
# The APKs (F-Droid's, of windows/test/compat/smoke.txt and windows/test/screens/gate.txt) are read by
# dex_refs.py for what they reference. Output: <work dir>/scan/*.tsv (apprefs.tsv with APKs), <work dir>/refs.tsv, <work dir>/report.md.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=$1; stage=$2; shift 2
mkdir -p "$work/sdk" "$work/scan"
fetch() { [ -s "$2" ] || curl -fsSL -o "$2.part" "$1" && { [ -s "$2" ] || mv "$2.part" "$2"; }; }
for p in 34:platform-34-ext7_r03 35:platform-35_r02 36:platform-36_r02; do
    l=${p%%:*}; f=${p#*:}
    if [ ! -s "$work/sdk/android-$l.jar" ]; then
        fetch "https://dl.google.com/android/repository/$f.zip" "$work/sdk/$f.zip"
        unzip -p "$work/sdk/$f.zip" "android-$l/android.jar" > "$work/sdk/android-$l.jar"
        [ "$l" = 36 ] && unzip -p "$work/sdk/$f.zip" "android-36/data/api-versions.xml" > "$work/sdk/api-versions.xml"
        rm -f "$work/sdk/$f.zip"
    fi
done
ALL=14-robolectric-10818077
fetch "https://repo1.maven.org/maven2/org/robolectric/android-all/$ALL/android-all-$ALL.jar" "$work/sdk/android-all-14.jar"
ASM=9.7.1
for a in asm asm-tree; do
    fetch "https://repo1.maven.org/maven2/org/ow2/asm/$a/$ASM/$a-$ASM.jar" "$work/sdk/$a.jar"
done
L=$stage/lib
refs=()
if [ $# -gt 0 ]; then
    python3 -I "$here/dex_refs.py" --out "$work/refs.tsv" "$@"
    refs=(--refs "$work/refs.tsv")
fi
java -Xmx3g -cp "$work/sdk/asm.jar:$work/sdk/asm-tree.jar" "$here/ApiGapScan.java" --out "$work/scan" \
    --versions "$work/sdk/api-versions.xml" \
    --api "$work/sdk/android-34.jar" --api "$work/sdk/android-35.jar" --api "$work/sdk/android-36.jar" \
    --jre "$stage/jre" --patch "$L/java_base_patch" --patch "$L/jdk_unsupported_patch" \
    --aosp "$work/sdk/android-all-14.jar" "${refs[@]}" \
    "$L/aosp-app.jar" "$L/aosp-views.jar" "$L/aosp-resources.jar" "$L/aosp-graphics.jar" \
    "$L/android-framework.jar" "$L/macapk-loader.jar" "$L/bcprov-jdk18on-1.79.jar"
# The in-process services' methods with no answer (the generated <Interface>Impl's calls of
# Services.unanswered). The runtime's own links are windows/scripts/LinkScan.java's (api-gaps.md, 2b).
java -cp "$work/sdk/asm.jar:$work/sdk/asm-tree.jar" "$here/UnansweredScan.java" \
    "$L/aosp-app.jar" "$L/aosp-views.jar" > "$work/scan/unanswered.tsv"
python3 -I "$here/report.py" --scan "$work/scan" > "$work/report.md"
