#!/usr/bin/env bash
# build_aosp_app.sh -- AOSP 14's own app layer, every app's process (docs/research/aosp-app.md), into
# a staged runtime:
#
#   <out>/lib/aosp-app.jar   AOSP 14's android.app lifecycle and its drivers: ActivityThread (with
#                            its H, ApplicationThread and ActivityClientRecord), the client
#                            transactions (android.app.servertransaction: LaunchActivityItem,
#                            ResumeActivityItem, PauseActivityItem... and TransactionExecutor),
#                            Instrumentation, ContextImpl, LoadedApk, Application, Activity,
#                            ActivityClient, ActivityTaskManager, ResourcesManager, the framework's
#                            Fragment/FragmentManager/LoaderManager, the AIDL interfaces between the
#                            app and the system (IActivityManager, IActivityTaskManager,
#                            IActivityClientController, IApplicationThread); the classes of
#                            android-all these name that Win.apk's framework (with AOSP's graphics,
#                            resources and views) lacks, from a bounded set of packages; and
#                            windows/java/aosp_app: the in-process activity manager, activity task
#                            manager and activity client controller that answer them, and Compat
#
#   windows/scripts/build_aosp_app.sh <out> [deps dir] [full android-framework.jar]
#
# <out> is a runtime staged by build_java.sh, build_aosp_gfx.sh, build_aosp_res.sh and
# build_aosp_views.sh: the app layer runs on AOSP's graphics, resources and views. The deps dir
# (default thirdparty/aosp-gfx) gets android-all 14 and ASM here if it lacks them. The closure,
# the bridge's compilation and LinkScan need the framework as compiled: a stage made by
# stage_runtime.sh has dropped the classes AOSP's resources and views shadow from its
# lib/android-framework.jar (marked by META-INF/winapk-aosp-shadowed.txt), so give the whole jar,
# which stage_runtime.sh leaves in <cmake build dir>/java/ (CI: the winapk-binaries artifact).
#
# Kept Win.apk's (not taken from android-all) for this step: android.content (Context, Intent...),
# ActivityManager (the public API of the activity service: memory, processes, tasks; AOSP's static
# getService() goes to the in-process service through Compat), Service, IntentService, NativeActivity
# (the host's native window), PendingIntent, the notifications, and every manager behind
# getSystemService: ContextImpl's lookups go to Win.apk's registry (Compat) instead of AOSP's
# SystemServiceRegistry, whose fetchers construct AOSP's managers over system_server.
#
# At build time AospAppRewrite.java (beside this script) gives the taken classes' natives Java bodies
# or a named UnsupportedOperationException, sends what they call that Win.apk's classes lack to
# com.winapk.aospapp.Compat, and generates the in-process services (AIDL Stub subclasses).
# LinkScan.java then lists every member reference that would still not link
# (<out>/.build/aosp-app.links.txt, printed as a count). stage_runtime.sh stages it; the host puts it
# first on the class path of every JVM with the framework (host/aosp_gfx.h, aosp_app_prepare), and
# windows/test/aospapp runs it on a plain JVM.

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
OUT="${1:?usage: build_aosp_app.sh <staged runtime> [deps dir] [full android-framework.jar]}"
DEPS="${2:-$HERE/../../thirdparty/aosp-gfx}"
OUT="$(cd -- "$OUT" && pwd)"
mkdir -p "$DEPS"
DEPS="$(cd -- "$DEPS" && pwd)"
FRAMEWORK="${3:-$OUT/lib/android-framework.jar}"
LOADER="$OUT/lib/macapk-loader.jar"
GFX="$OUT/lib/aosp-graphics.jar"
RES="$OUT/lib/aosp-resources.jar"
VIEWS="$OUT/lib/aosp-views.jar"
for f in "$FRAMEWORK" "$LOADER"; do
    [[ -f "$f" ]] || { echo "[aosp-app] no $f: run build_java.sh first" >&2; exit 1; }
done
# By the listing, not zipinfo's exit code (MSYS2's answers 0 for a name that is not there); no grep -q,
# whose early exit would kill unzip with SIGPIPE under pipefail.
if unzip -Z1 "$FRAMEWORK" | grep -x "META-INF/winapk-aosp-shadowed.txt" > /dev/null; then
    echo "[aosp-app] $FRAMEWORK lacks the classes stage_runtime.sh dropped: give the whole jar" \
         "(<cmake build dir>/java/android-framework.jar) as the third argument" >&2
    exit 1
fi
[[ -f "$GFX" ]] || { echo "[aosp-app] no $GFX: run build_aosp_gfx.sh first" >&2; exit 1; }
[[ -f "$RES" ]] || { echo "[aosp-app] no $RES: run build_aosp_res.sh first" >&2; exit 1; }
[[ -f "$VIEWS" ]] || { echo "[aosp-app] no $VIEWS: run build_aosp_views.sh first" >&2; exit 1; }

if [[ -n "${JAVA_HOME:-}" ]]; then
    JAVA="$JAVA_HOME/bin/java" JAVAC="$JAVA_HOME/bin/javac" JAR="$JAVA_HOME/bin/jar" JDEPS="$JAVA_HOME/bin/jdeps"
else
    JAVA=java JAVAC=javac JAR=jar JDEPS=jdeps
fi
native_path() { if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }
if command -v cygpath > /dev/null 2>&1; then PS=';'; else PS=':'; fi
cp_of() { local out="" p; for p in "$@"; do out="${out:+$out$PS}$(native_path "$p")"; done; printf '%s' "$out"; }

# android-all only ("jar"): the native runtime in the deps dir, whichever platform's, is left alone.
"$HERE/../test/aospgfx/fetch.sh" "$DEPS" jar > /dev/null
ALL="$DEPS/android-all-14.jar"
MAVEN=https://maven-central.storage-download.googleapis.com/maven2
mkdir -p "$DEPS/jars"
fetch() {  # group-path artifact version sha256 (as build_aosp_res.sh)
    local f="$DEPS/jars/$2-$3.jar"
    if [[ -f "$f" ]] && echo "$4  $f" | sha256sum -c --status; then return 0; fi
    echo "[aosp-app] fetching $2-$3"
    curl -fL --retry 5 --retry-delay 5 --retry-all-errors -sS -o "$f.part" "$MAVEN/$1/$2/$3/$2-$3.jar"
    if ! echo "$4  $f.part" | sha256sum -c --status; then
        echo "[aosp-app] SHA-256 mismatch for $2-$3: got $(sha256sum "$f.part" | cut -d' ' -f1)" >&2
        exit 1
    fi
    mv "$f.part" "$f"
}
fetch org/ow2/asm asm 9.8 876eab6a83daecad5ca67eb9fcabb063c97b5aeb8cf1fca7a989ecde17522051
fetch org/ow2/asm asm-commons 9.8 3301a1c1cb4c59fcc5292648dac1d7c5aed4c0f067dfbe88873b8cdfe77404f4
ASM="$(cp_of "$DEPS/jars/asm-9.8.jar" "$DEPS/jars/asm-commons-9.8.jar")"

WORK="$OUT/.build/aosp-app"
CLASSES="$WORK/classes"
rm -rf "$WORK"
mkdir -p "$CLASSES"

echo "[aosp-app] AOSP's classes from android-all 14"
unzip -Z1 "$ALL" > "$WORK/all.txt"
# The lifecycle and what drives it in the app's process: aosp_app_take.sh's list, which build_java.sh
# compiles the framework against.
# shellcheck source=aosp_app_take.sh
source "$HERE/aosp_app_take.sh"
aosp_app_take "$WORK/all.txt" > "$WORK/take.txt"
extract() { (cd "$CLASSES" && xargs -d '\n' unzip -qo "$1" < "$2"); }
extract "$ALL" "$WORK/take.txt"
echo "[aosp-app]   $(wc -l < "$WORK/take.txt") classes of android.app's lifecycle"

# What those classes name that Win.apk's framework (with AOSP's graphics, resources and views) lacks,
# until nothing more is missing, as build_aosp_views.sh does, from AOSP's own plumbing only: never
# android.app's managers (SystemServiceRegistry and what it constructs), android.content, ICU, media
# or telephony, nor android.os's core.
ALLOW='^(android/app/([A-Z]|servertransaction/)|android/content/(I[A-Z]|pm/(I[A-Z]|ParceledListSlice|BaseParceledListSlice|ProviderInfoList|split/))|android/content/res/loader/|android/window/|com/android/internal/(app/IVoiceInteractor|content/ReferrerIntent|os/(SomeArgs|TransferPipe|HandlerCaller)|policy/|util/)|android/util/|android/os/(SharedMemory|StrictMode[$]|ServiceManager|IProgressListener|PerformanceCollector)|android/view/|android/hardware/display/)'
DENY='^(com/android/internal/(telephony|os/(Zygote|RuntimeInit))|android/content/res/[A-Z]|android/app/(SystemServiceRegistry|ActivityManager|Service|IntentService|NativeActivity|PendingIntent|Notification|ApplicationPackageManager|.*Manager)([$.]|$)|android/os/(Parcel|Binder|Looper|MessageQueue|Handler|Build|SystemProperties|Trace|Process)[$.])'
{ unzip -Z1 "$VIEWS"; unzip -Z1 "$RES"; unzip -Z1 "$GFX"; } | tr -d '\r' | grep '\.class$' > "$WORK/aosp-jars.txt"
for round in $(seq 1 30); do
    { (cd "$CLASSES" && find . -name '*.class' | sed 's#^[.]/##'); cat "$WORK/aosp-jars.txt"; } > "$WORK/have.txt"
    "$JDEPS" --multi-release 21 -verbose:class -filter:none -cp "$(cp_of "$VIEWS" "$RES" "$GFX" "$FRAMEWORK" "$LOADER")" \
        "$CLASSES" 2> /dev/null |
        tr -d '\r' | awk '$NF == "found" && $(NF-1) == "not" { print $3 }' | sort -u |
        tr . / | grep -E "$ALLOW" | grep -vE "$DENY" |
        awk -v have="$WORK/have.txt" 'BEGIN { while ((getline l < have) > 0) h[l] = 1 }
            { o = $0; sub(/[$].*/, "", o); if (o == $0 || h[o ".class"]) print $0 ".class" }' \
        > "$WORK/need.txt" || true
    grep -xFf "$WORK/need.txt" "$WORK/all.txt" > "$WORK/more.txt" || true
    [[ -s "$WORK/more.txt" ]] || break
    extract "$ALL" "$WORK/more.txt"
    echo "[aosp-app]   + $(wc -l < "$WORK/more.txt") classes they need that the framework lacks"
done

if [[ -d "$HERE/../java/aosp_app" ]]; then
    echo "[aosp-app] windows/java/aosp_app"
    find "$HERE/../java/aosp_app" -name '*.java' | sort | while read -r f; do native_path "$f"; done > "$WORK/src.txt"
    # AOSP's classes (android-all) before Win.apk's of the same name: the bridge is written against
    # AOSP's API; com.macapk.* and com.winapk.* come from Win.apk's jars.
    "$JAVAC" -nowarn -encoding UTF-8 --release 21 -d "$CLASSES" \
        -cp "$(cp_of "$CLASSES" "$VIEWS" "$RES" "$ALL" "$FRAMEWORK" "$LOADER")" @"$WORK/src.txt"
fi

echo "[aosp-app] the rewrite (AospAppRewrite.java)"
"$JAVA" -cp "$ASM" "$(native_path "$HERE/AospAppRewrite.java")" "$(native_path "$CLASSES")"

rm -f "$OUT/lib/aosp-app.jar"
( cd "$CLASSES" && "$JAR" --create --file "$OUT/lib/aosp-app.jar" . )
echo "[aosp-app] $OUT/lib/aosp-app.jar: $(find "$CLASSES" -name '*.class' | wc -l) classes"

# What would still not link, as the JVM resolves it (aosp-app.jar first, as the host would put it).
"$JAVA" -cp "$ASM" "$(native_path "$HERE/LinkScan.java")" \
    "$(native_path "$OUT/lib/aosp-app.jar")" "$(native_path "$OUT/lib/aosp-app.jar")" "$(native_path "$VIEWS")" \
    "$(native_path "$RES")" "$(native_path "$GFX")" "$(native_path "$FRAMEWORK")" "$(native_path "$LOADER")" \
    > "$WORK/links.txt"
cp "$WORK/links.txt" "$OUT/.build/aosp-app.links.txt"
echo "[aosp-app]   $(grep -c '^[MF] ' "$WORK/links.txt" || true) member references and" \
     "$(grep -c '^C ' "$WORK/links.txt" || true) classes of AOSP's app layer that do not link" \
     "($OUT/.build/aosp-app.links.txt; reached only when that code runs)"
rm -rf "$WORK"
du -sh "$OUT/lib/aosp-app.jar" | sed 's/^/[aosp-app] /'
