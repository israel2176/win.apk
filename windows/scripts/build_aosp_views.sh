#!/usr/bin/env bash
# build_aosp_views.sh -- what AOSP's own views, the only ones apps run on, need (host/aosp_gfx.h;
# docs/research/aosp-views.md, step V1; windows/README.md, "AOSP views"), into a staged runtime:
#
#   <out>/lib/aosp-views.jar   AOSP 14's android.view.** (View, ViewGroup, LayoutInflater,
#                              ViewRootImpl, Choreographer, WindowManagerGlobal, the AIDL interfaces
#                              of the window manager and its session, inputmethod, accessibility...),
#                              android.widget.**, com.android.internal.{view,widget,policy}.**
#                              (PhoneWindow, DecorView, PhoneLayoutInflater); the classes of
#                              android-all these name that Win.apk's framework lacks, from a bounded
#                              set of packages (below); and windows/java/aosp_views: the second
#                              ViewHost (AospViewHost), the in-process window manager and its session,
#                              the bodies of the natives that have one here
#
#   windows/scripts/build_aosp_views.sh <out> [deps dir]
#
# <out> is a runtime staged by build_java.sh, build_aosp_gfx.sh and build_aosp_res.sh (AOSP's views
# run on AOSP's graphics and resources). The deps dir (default thirdparty/aosp-gfx) is the one
# build_aosp_res.sh fills (android-all 14, ASM).
#
# Kept Win.apk's (not taken from android-all), as the graphics work and the host need them: Surface,
# SurfaceControl, SurfaceView, TextureView, SurfaceHolder (the host's GL and video surfaces);
# MotionEvent, KeyEvent, KeyCharacterMap, VelocityTracker, InputDevice, InputQueue (the host's input,
# Java here, natives on Android); ThreadedRenderer (aosp-graphics.jar's). android.animation,
# android.transition and android.app stay Win.apk's.
#
# At build time AospViewsRewrite.java (beside this script) gives the natives of the taken classes
# Java bodies (com.winapk.aospviews.Native<Class>, or a named UnsupportedOperationException), and
# sends what AOSP's classes call that Win.apk's lack to com.winapk.aospviews.Compat. LinkScan.java
# then lists every member reference that would still not link (lib/aosp-views.links.txt in the
# build's scratch folder, printed as a count). stage_runtime.sh runs this after build_aosp_res.sh.

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
OUT="${1:?usage: build_aosp_views.sh <staged runtime> [deps dir]}"
DEPS="${2:-$HERE/../../thirdparty/aosp-gfx}"
OUT="$(cd -- "$OUT" && pwd)"
mkdir -p "$DEPS"
DEPS="$(cd -- "$DEPS" && pwd)"
FRAMEWORK="$OUT/lib/android-framework.jar"
LOADER="$OUT/lib/macapk-loader.jar"
GFX="$OUT/lib/aosp-graphics.jar"
RES="$OUT/lib/aosp-resources.jar"
for f in "$FRAMEWORK" "$LOADER"; do
    [[ -f "$f" ]] || { echo "[aosp-views] no $f: run build_java.sh first" >&2; exit 1; }
done
# The closure below is what the whole framework lacks: a framework stage_runtime.sh has already
# dropped the classes AOSP's resources and views shadow from would make it take those too.
# By the listing, not zipinfo's exit code (MSYS2's answers 0 for a name that is not there); no grep -q,
# whose early exit would kill unzip with SIGPIPE under pipefail.
if unzip -Z1 "$FRAMEWORK" | grep -x "META-INF/winapk-aosp-shadowed.txt" > /dev/null; then
    echo "[aosp-views] $FRAMEWORK lacks the classes stage_runtime.sh dropped: run build_java.sh again" >&2
    exit 1
fi
[[ -f "$GFX" ]] || { echo "[aosp-views] no $GFX: run build_aosp_gfx.sh first" >&2; exit 1; }
[[ -f "$RES" ]] || { echo "[aosp-views] no $RES: run build_aosp_res.sh first" >&2; exit 1; }

if [[ -n "${JAVA_HOME:-}" ]]; then
    JAVA="$JAVA_HOME/bin/java" JAVAC="$JAVA_HOME/bin/javac" JAR="$JAVA_HOME/bin/jar" JDEPS="$JAVA_HOME/bin/jdeps"
else
    JAVA=java JAVAC=javac JAR=jar JDEPS=jdeps
fi
native_path() { if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }
if command -v cygpath > /dev/null 2>&1; then PS=';'; else PS=':'; fi
cp_of() { local out="" p; for p in "$@"; do out="${out:+$out$PS}$(native_path "$p")"; done; printf '%s' "$out"; }

"$HERE/../test/aospgfx/fetch.sh" "$DEPS" windows
ALL="$DEPS/android-all-14.jar"
ASM_JAR="$DEPS/jars/asm-9.8.jar"
ASM_COMMONS="$DEPS/jars/asm-commons-9.8.jar"
for f in "$ASM_JAR" "$ASM_COMMONS"; do
    [[ -f "$f" ]] || { echo "[aosp-views] no $f: build_aosp_res.sh fetches it" >&2; exit 1; }
done
ASM="$(cp_of "$ASM_JAR" "$ASM_COMMONS")"

WORK="$OUT/.build/aosp-views"
CLASSES="$WORK/classes"
rm -rf "$WORK"
mkdir -p "$CLASSES"

echo "[aosp-views] AOSP's classes from android-all 14"
unzip -Z1 "$ALL" > "$WORK/all.txt"
TAKE='^(android/view/.*|android/widget/.*|com/android/internal/(view|widget|policy)/.*)\.class$'
# Win.apk's own, kept (see the header). A nested class goes with its outer one.
# ThreadedRenderer is aosp-graphics.jar's (its outer class only: its nested classes, which
# ViewRootImpl implements, are android-all's, taken here).
KEEP='^android/view/((Surface|SurfaceControl|SurfaceView|SurfaceHolder|TextureView|MotionEvent|KeyEvent|KeyCharacterMap|VelocityTracker|InputDevice|InputQueue|IGraphicsStats|IGraphicsStatsCallback|NativeVectorDrawableAnimator)([$].*)?|ThreadedRenderer)\.class$'
# And a few classes outside those packages whose AOSP version the views need and Win.apk's lacks
# members of, where no Win.apk class needs one AOSP's lacks (checked with LinkScan): StateSet
# (View's drawable states, StateSet.get and the VIEW_STATE_* sets),
# android.R.styleable (the views read the framework's styleables through it; Win.apk's android.R
# has none), android.text.method (TextView's key listeners and movement methods: a hardware
# keyboard's keys become text through TextKeyListener/QwertyKeyListener, which Win.apk's leave to the
# host's text path, and AllCapsTransformationMethod is built with a Context; CharacterPickerDialog),
# android.text.Selection (Editor's PositionIterator), and android.window's back dispatch
# (WindowOnBackInvokedDispatcher and its callbacks: ViewRootImpl's, the Win.apk copies lack what it
# calls) and SurfaceSyncGroup (ViewRootImpl's draw syncs); Win.apk's SplashScreen,
# SplashScreenView, InputTransferToken and TrustedPresentationThresholds stay.
# Dialogs are AOSP's too (Dialog, AlertDialog and its AlertController, ProgressDialog, the date
# and time pickers' dialogs, Presentation, CharacterPickerDialog): a dialog is a PhoneWindow added
# to the window manager, which Win.apk's Dialog (its own Window, drawn by its overlay stack)
# cannot be under AOSP's ViewRootImpl. docs/research/aosp-views.md, "V1 as built". With them
# WindowDecorActionBar (and its NavItemSelectedListener): a dialog whose theme has an action bar
# shows it (Dialog.show), and without the class Dialog did not link (a CDS dump's verification).
REPLACE='^(android/app/(Dialog|AlertDialog|ProgressDialog|DatePickerDialog|TimePickerDialog|Presentation)([$].*)?|android/text/method/[^/]*|com/android/internal/app/(AlertController|MicroAlertController|WindowDecorActionBar|NavItemSelectedListener)([$].*)?|android/util/StateSet|android/R[$]styleable|android/text/Selection([$].*)?|android/window/(WindowOnBackInvokedDispatcher|OnBackInvokedDispatcher|OnBackInvokedCallback|OnBackAnimationCallback|CompatOnBackInvokedCallback|BackEvent|SystemOnBackInvokedCallbacks|SurfaceSyncGroup)([$].*)?)\.class$'
grep -E "$TAKE|$REPLACE" "$WORK/all.txt" | grep -vE "$KEEP" > "$WORK/take.txt"
extract() { (cd "$CLASSES" && xargs -d '\n' unzip -qo "$1" < "$2"); }
extract "$ALL" "$WORK/take.txt"
echo "[aosp-views]   $(wc -l < "$WORK/take.txt") classes of android.view, android.widget, com.android.internal.{view,widget,policy}"

# What those classes name that Win.apk's framework (with AOSP's graphics and resources) lacks, until
# nothing more is missing: only from these packages (AOSP's own plumbing that Win.apk has no class
# of). Not android.app, android.animation, android.content.pm/res, ICU, media or telephony, nor
# android.os's core (Parcel, Binder, Looper...): Win.apk's own. A reference left unresolved is only
# an error if the code that makes it runs (HotSpot links lazily); LinkScan lists them.
ALLOW='^(com/google/android/collect/|android/app/PropertyInvalidatedCache|android/app/(INotificationManager|ITransientNotification|ITransientNotificationCallback)([$]|$)|android/animation/(AnimationHandler|RevealAnimator)|android/view/|android/widget/|android/window/|com/android/internal/|android/util/|android/text/|android/os/|android/hardware/display/|android/hardware/input/|android/hardware/graphics/common/|android/accessibilityservice/|android/content/|android/graphics/|android/sysprop/|android/service/autofill/|android/service/textclassifier/)'
DENY='^(com/android/internal/(telephony|app|os/(Zygote|RuntimeInit)|content/om)|android/content/(pm|res)/|android/os/(Parcel|Binder|Looper|MessageQueue|Handler|Build|SystemProperties|Trace|Process)[$.])'
# The other AOSP jars' classes (grep, not an unzip pattern: MSYS2's unzip matched no `*.class`).
{ unzip -Z1 "$RES"; unzip -Z1 "$GFX"; } | tr -d '\r' | grep '\.class$' > "$WORK/aosp-jars.txt"
for round in $(seq 1 30); do
    # A nested class goes with its outer one: taken when the outer is here or one of AOSP's in
    # aosp-resources.jar / aosp-graphics.jar (the same android-all 14: FastPrintWriter is the
    # graphics', its DummyWriter came from nowhere and the class did not link).
    { (cd "$CLASSES" && find . -name '*.class' | sed 's#^[.]/##'); cat "$WORK/aosp-jars.txt"; } > "$WORK/have.txt"
    "$JDEPS" --multi-release 21 -verbose:class -filter:none -cp "$(cp_of "$RES" "$GFX" "$FRAMEWORK" "$LOADER")" \
        "$CLASSES" 2> /dev/null |
        tr -d '\r' | awk '$NF == "found" && $(NF-1) == "not" { print $3 }' | sort -u |
        tr . / | grep -E "$ALLOW" | grep -vE "$DENY" |
        awk -v have="$WORK/have.txt" 'BEGIN { while ((getline l < have) > 0) h[l] = 1 }
            { o = $0; sub(/[$].*/, "", o); if (o == $0 || h[o ".class"]) print $0 ".class" }' \
        > "$WORK/need.txt" || true
    grep -xFf "$WORK/need.txt" "$WORK/all.txt" | grep -vE "$KEEP" > "$WORK/more.txt" || true
    [[ -s "$WORK/more.txt" ]] || break
    extract "$ALL" "$WORK/more.txt"
    echo "[aosp-views]   + $(wc -l < "$WORK/more.txt") classes they need that the framework lacks"
done

# RippleDrawable (aosp-graphics.jar's): a pressed view's patterned ripple builds its RippleShader
# even on a software canvas, which then skips drawing it; the native runtime's SkSL compiler is
# older than AOSP 14's shader and refuses it, which threw out of the draw. The rewrite starts that
# ripple on a hardware canvas only (AospViewsRewrite.java): what a software canvas shows is the same.
(cd "$CLASSES" && unzip -qo "$GFX" 'android/graphics/drawable/RippleDrawable.class')

echo "[aosp-views] windows/java/aosp_views"
find "$HERE/../java/aosp_views" -name '*.java' | sort | while read -r f; do native_path "$f"; done > "$WORK/src.txt"
# AOSP's classes (android-all) before Win.apk's of the same name: the bridge is written against
# AOSP's API; com.macapk.* comes from Win.apk's jars.
"$JAVAC" -nowarn -encoding UTF-8 --release 21 -d "$CLASSES" \
    -cp "$(cp_of "$CLASSES" "$RES" "$ALL" "$FRAMEWORK" "$LOADER")" @"$WORK/src.txt"

echo "[aosp-views] the rewrite (AospViewsRewrite.java)"
"$JAVA" -cp "$ASM" "$(native_path "$HERE/AospViewsRewrite.java")" "$(native_path "$CLASSES")"

# Android 15's members of the views AOSP 14 lacks, which apps built for the runtime's SDK level (35)
# call: each public static method of Api35 becomes a method of its first parameter's class
# (AospAddMethods.java, as build_aosp_gfx.sh does for the graphics).
echo "[aosp-views] API 35 methods added to AOSP's classes"
"$JAVA" "$(native_path "$HERE/AospAddMethods.java")" "$(native_path "$CLASSES")" com/winapk/aospviews/Api35

rm -f "$OUT/lib/aosp-views.jar"
( cd "$CLASSES" && "$JAR" --create --file "$OUT/lib/aosp-views.jar" . )
echo "[aosp-views] $OUT/lib/aosp-views.jar: $(find "$CLASSES" -name '*.class' | wc -l) classes"

# What would still not link, as the JVM resolves it (the class path order the host uses).
"$JAVA" -cp "$ASM" "$(native_path "$HERE/LinkScan.java")" \
    "$(native_path "$OUT/lib/aosp-views.jar")" "$(native_path "$OUT/lib/aosp-views.jar")" \
    "$(native_path "$RES")" "$(native_path "$GFX")" "$(native_path "$FRAMEWORK")" "$(native_path "$LOADER")" \
    > "$WORK/links.txt"
cp "$WORK/links.txt" "$OUT/.build/aosp-views.links.txt"
echo "[aosp-views]   $(grep -c '^[MF] ' "$WORK/links.txt" || true) member references of AOSP's classes" \
     "that do not link ($OUT/.build/aosp-views.links.txt; reached only when that code runs)"
rm -rf "$WORK"
du -sh "$OUT/lib/aosp-views.jar" | sed 's/^/[aosp-views] /'
