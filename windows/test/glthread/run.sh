#!/usr/bin/env bash
# Runs the GL thread test (build.sh's glthread.apk) in a window, one activity at a time, and checks
# what Android guarantees a renderer whose surface does not exist yet when it is set:
#   - onSurfaceCreated, onSurfaceChanged and onDrawFrame (the app's own copy: its b, a and c) all on
#     the view's GL thread, never on the host's render thread;
#   - after the window is resized (WINAPK_TEST_RESIZE, a second after the first frame) the renderer
#     hears onSurfaceChanged at the new width: the same renderer for ForkActivity, which keeps its
#     window across the change (configChanges), a relaunched activity's for the other two;
#   - the GL thread never dies (an EGL refusal used to kill an app's own one);
#   - for the app's own GL threads, a loader thread on a context shared with the GL thread uploads
#     textures the whole time, from before the window surface exists and across the resize, where
#     the GL thread remakes its window surface, as Flutter's IO and raster threads do; the run must
#     not crash (the host's render tick once drew on the app's display beside them).
#
#   windows/test/glthread/run.sh <winapk-runtime.exe> <glthread.apk> <out dir>
#
# The runtime finds its JRE as it always does (stage/jre, WINAPK_JAVA_HOME or JAVA_HOME). On
# Windows (MSYS2) paths go to the runtime as Windows paths. Exit 0 when every check holds.
set -euo pipefail

RUNTIME="${1:?usage: run.sh <winapk-runtime.exe> <glthread.apk> <out dir>}"
APK="${2:?usage: run.sh <winapk-runtime.exe> <glthread.apk> <out dir>}"
OUT="${3:?usage: run.sh <winapk-runtime.exe> <glthread.apk> <out dir>}"
mkdir -p "$OUT"
native() { if command -v cygpath > /dev/null 2>&1; then cygpath -w "$1"; else printf '%s\n' "$1"; fi; }
WIDTH=500

fail=0
check() {  # check <log> <what> <extended regex>
    if grep -aqE "$3" "$1"; then echo "  ok: $2"; else echo "  MISSING: $2 ($3)"; fail=1; fi
}
for act in PlatformActivity ForkActivity Fork14Activity; do
    log="$OUT/$act.log"
    rm -rf "$OUT/$act-data"
    MACAPK_LAUNCH_ACTIVITY="dev.winapk.glthread.$act" WINAPK_HEADLESS_EXIT_AFTER_FRAMES=300 \
        WINAPK_TEST_RESIZE="${WIDTH}x700" timeout 600 "$RUNTIME" --apk "$(native "$APK")" \
        --data "$(native "$OUT/$act-data")" > "$log" 2>&1 || { tail -n 100 "$log"; fail=1; continue; }
    echo "[glthread] $act"
    case "$act" in
        PlatformActivity)
            check "$log" "onSurfaceCreated on the GL thread" "GLPROBE: platform onSurfaceCreated on GLThread"
            check "$log" "onDrawFrame on the GL thread" "GLPROBE: platform onDrawFrame #1 on GLThread"
            check "$log" "onSurfaceChanged at the resized width" "GLPROBE: platform onSurfaceChanged ${WIDTH}x"
            ;;
        ForkActivity | Fork14Activity)
            name=$([[ "$act" == ForkActivity ]] && echo fork || echo fork14)
            check "$log" "created on its GL thread" "GLPROBE: $name created on GLThread $name"
            check "$log" "drew on its GL thread" "GLPROBE: $name drew #1 on GLThread $name"
            check "$log" "changed at the resized width" "GLPROBE: $name changed ${WIDTH}x[0-9]+ on GLThread $name"
            check "$log" "its loader uploaded on its own thread" \
                "GLPROBE: $name loader uploaded #300 on GLThread $name loader"
            ;;
    esac
    if grep -aE "GLPROBE: .* died" "$log"; then fail=1; fi
done
exit "$fail"
