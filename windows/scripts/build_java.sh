#!/usr/bin/env bash
# build_java.sh -- builds upstream's Java side (runtime/java and friends) with a
# stock JDK 21, on Windows (MSYS2) or Linux. The output is what the Windows host
# puts on the JVM's classpath:
#
#   <out>/lib/android-framework.jar   the Android framework reimplementation
#   <out>/lib/macapk-loader.jar       boot code, the DEX-to-JVM converter and
#                                     apksig (thirdparty/apksig/README.md)
#   <out>/lib/bcprov-jdk18on-1.79.jar the "BC" security provider Android has (unsigned, below)
#   <out>/lib/winapk-tools.jar        Windows-side helpers (windows/java)
#   <out>/lib/java_base_patch/        classes patched into java.base (upstream's
#                                     runtime/java_base_patch and Win.apk's
#                                     windows/java/java_base_patch)
#   <out>/lib/jdk_unsupported_patch/  classes patched into jdk.unsupported
#   <out>/lib/winapk-framework.classlist  the classes the framework archive
#                                     holds (host/cds.h; windows/java's copy)
#
#   windows/scripts/build_java.sh [out]       (default: windows/build/runtime)
#
# This mirrors the Java half of runtime/scripts/build.sh, which also builds the
# Mac native runtime and so only runs on macOS. Left out on purpose: the pairip
# reference files and the platform resource APK (both need macOS tooling; the
# resource APK is a later step). Keep the two in sync when upstream changes
# its Java build.
#
# The framework has no android.graphics of its own (nor the android.text
# classes that measure and lay out): it runs on AOSP's, lib/aosp-graphics.jar
# (build_aosp_gfx.sh), and compiles against the same classes of android-all 14
# (aosp_gfx_take.sh), with the methods build_aosp_gfx.sh adds to them, so that
# it calls nothing they lack. android-all comes from windows/test/aospgfx/fetch.sh
# into WINAPK_AOSP_GFX_DEPS (default thirdparty/aosp-gfx, which CI caches).
#
# JAVA_HOME selects the JDK (21); otherwise javac and jar come from PATH.

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
RT="$(cd -- "$HERE/../../runtime" >/dev/null 2>&1 && pwd)"
OUT="${1:-$HERE/../build/runtime}"
mkdir -p "$OUT"
OUT="$(cd -- "$OUT" && pwd)"  # absolute: the packaging steps cd elsewhere
LIB="$OUT/lib"
WORK="$OUT/.build"  # scratch: classes, file lists, javac log
TMP="$WORK/classes"

if [[ -n "${JAVA_HOME:-}" ]]; then
    JAVA="$JAVA_HOME/bin/java"
    JAVAC="$JAVA_HOME/bin/javac"
    JAR="$JAVA_HOME/bin/jar"
else
    JAVA=java
    JAVAC=javac
    JAR=jar
fi

# javac reads the @file lists itself, so on MSYS2, where only command-line
# arguments are translated to Windows paths, the lists need Windows paths.
native_paths() {
    if command -v cygpath > /dev/null 2>&1; then cygpath -m -f -; else cat; fi
}
# The same for a path inside an argument ("module=path"), which MSYS2 does not
# always recognize.
native_path() { printf '%s\n' "$1" | native_paths; }

# A stale class from a deleted source would otherwise ship forever; javac
# recompiles everything anyway, so wiping costs nothing.
rm -rf "$TMP"
mkdir -p "$TMP" "$LIB"

# AOSP's graphics classes, to compile against only: they ship in aosp-graphics.jar, never in
# android-framework.jar (javac writes no class it only reads from the class path).
echo "[build_java] AOSP's graphics classes to compile against (android-all 14)..."
DEPS="${WINAPK_AOSP_GFX_DEPS:-$HERE/../../thirdparty/aosp-gfx}"
"$HERE/../test/aospgfx/fetch.sh" "$DEPS" jar > /dev/null
DEPS="$(cd -- "$DEPS" && pwd)"
ALL="$DEPS/android-all-14.jar"
GFX_API="$WORK/aosp-gfx-api"
rm -rf "$GFX_API"
mkdir -p "$GFX_API"
# shellcheck source=aosp_gfx_take.sh
source "$HERE/aosp_gfx_take.sh"
unzip -Z1 "$ALL" > "$WORK/all.txt"
aosp_gfx_take "$WORK/all.txt" > "$WORK/gfx-take.txt"
( cd "$GFX_API" && xargs -d '\n' unzip -qo "$ALL" < "$WORK/gfx-take.txt" )
# The members build_aosp_gfx.sh adds to AOSP's classes (Api35, FrameworkMembers), added here too.
"$JAVAC" -nowarn --release 21 -d "$(native_path "$GFX_API")" -cp "$(native_path "$ALL")" \
    "$(native_path "$HERE/../java/aosp_gfx/com/winapk/aospgfx/Api35.java")" \
    "$(native_path "$HERE/../java/aosp_gfx/com/winapk/aospgfx/FrameworkMembers.java")"
"$JAVA" "$(native_path "$HERE/AospAddMethods.java")" "$(native_path "$GFX_API")" \
    com/winapk/aospgfx/Api35 com/winapk/aospgfx/FrameworkMembers > /dev/null

# AOSP's app layer, to compile against only, likewise: it ships in aosp-app.jar (build_aosp_app.sh);
# the framework has no Activity, Application, Instrumentation, LoadedApk, ActivityThread or framework
# Fragment of its own.
echo "[build_java] AOSP's app layer classes to compile against (android-all 14)..."
APP_API="$WORK/aosp-app-api"
rm -rf "$APP_API"
mkdir -p "$APP_API"
# shellcheck source=aosp_app_take.sh
source "$HERE/aosp_app_take.sh"
aosp_app_take "$WORK/all.txt" > "$WORK/app-take.txt"
( cd "$APP_API" && xargs -d '\n' unzip -qo "$ALL" < "$WORK/app-take.txt" )

# A few classes of AOSP's views that Win.apk's own classes name in a declaration, to compile against
# only, likewise: they ship in aosp-views.jar (build_aosp_views.sh's closure). AccessibilityService's
# Callbacks and IAccessibilityServiceClientWrapper and InputManager's KeyboardBacklightListener are
# there because AOSP's classes name them: reflection on those builds every type their members name
# (ReflectScan.java).
VIEWS_API="$WORK/aosp-views-api"
rm -rf "$VIEWS_API"
mkdir -p "$VIEWS_API"
VIEWS_API_RE='^(android/accessibilityservice/(IAccessibilityServiceClient|IAccessibilityServiceConnection)'
VIEWS_API_RE+='|com/android/internal/inputmethod/(IAccessibilityInputMethodSessionCallback|IAccessibilityInputMethodSession'
VIEWS_API_RE+='|IRemoteAccessibilityInputConnection|RemoteAccessibilityInputConnection|CancellationGroup)'
VIEWS_API_RE+='|android/hardware/input/KeyboardBacklightState)([$].*)?\.class$'
grep -E "$VIEWS_API_RE" \
    "$WORK/all.txt" > "$WORK/views-api.txt"
( cd "$VIEWS_API" && xargs -d '\n' unzip -qo "$ALL" < "$WORK/views-api.txt" )

echo "[build_java] compiling runtime/java..."
LIST="$WORK/java.list"
# Paths go into an @file, which splits on spaces; upstream skips Finder
# conflict copies ("Foo 2.java") the same way.
find "$RT/java" -name '*.java' ! -path '* [2-9].java' | sort | native_paths > "$LIST"
# Upstream's sources produce a few dozen warnings (sun.misc.Unsafe use) that
# -nowarn does not silence, so the output goes to a log shown only on failure.
# Android's org.apache.http.legacy (thirdparty/apache-http-legacy/README.md):
# the real library, converted to JVM classes. The framework's boot-classpath
# slice of org.apache.http compiles against it, and its classes are unpacked
# into android-framework.jar (never over a class compiled here), as upstream's
# build does.
APACHE_LEGACY="$RT/../thirdparty/apache-http-legacy/org.apache.http.legacy.jvm.jar"
# AOSP's APK signature verifier (thirdparty/apksig/README.md): the loader's
# ApkVerification calls it, and its classes go into macapk-loader.jar.
APKSIG="$RT/../thirdparty/apksig/apksig-9.5.1.jar"
CP_SEP=":"
command -v cygpath > /dev/null 2>&1 && CP_SEP=";"  # javac on Windows splits on ';'
CP="$(native_path "$APKSIG")$CP_SEP$(native_path "$GFX_API")$CP_SEP$(native_path "$APP_API")"
CP="$CP$CP_SEP$(native_path "$VIEWS_API")"
[[ -f "$APACHE_LEGACY" ]] && CP="$CP$CP_SEP$(native_path "$APACHE_LEGACY")"
if ! "$JAVAC" -J-Xmx2g -d "$TMP" --release 21 -nowarn -Xlint:-options -cp "$CP" @"$LIST" \
        > "$WORK/javac.log" 2>&1; then
    cat "$WORK/javac.log" >&2
    echo "[build_java] javac failed" >&2
    exit 1
fi
if [[ -f "$APACHE_LEGACY" ]]; then
    # unzip -n: never overwrite a class compiled above.
    ( cd "$TMP" && unzip -qn "$APACHE_LEGACY" -x 'META-INF/*' )
fi
( cd "$TMP" && unzip -qn "$APKSIG" 'com/android/apksig/*' && mkdir -p apksig-legal && unzip -qo -d apksig-legal "$APKSIG" LICENSE )

# The resolver provider is a separate pass: it needs the running JDK's API
# rather than --release 21.
if [[ -d "$RT/net_spi" ]]; then
    find "$RT/net_spi" -name '*.java' ! -path '* [2-9].java' | sort | native_paths > "$WORK/net_spi.list"
    if [[ -s "$WORK/net_spi.list" ]]; then
        "$JAVAC" -d "$TMP" --source 21 --target 21 -nowarn -Xlint:-options @"$WORK/net_spi.list"
    fi
    if [[ -d "$RT/net_spi/META-INF/services" ]]; then
        mkdir -p "$TMP/META-INF/services"
        cp -f "$RT"/net_spi/META-INF/services/* "$TMP/META-INF/services/"
    fi
fi

# Classes the JDK only accepts from inside its own modules: upstream's
# overlay, plus Win.apk's own under windows/java (java.base only), compiled
# together into one directory the runtime hands to --patch-module.
patch_module() {  # patch_module <module> <out dir> <source dir>...
    local module="$1" dst="$2" src dirs="" sep=":"
    shift 2
    command -v cygpath > /dev/null 2>&1 && sep=";"  # javac on Windows splits on ';'
    local win_system=""
    if ! command -v cygpath > /dev/null 2>&1 && [[ "$module" == java.base && -f "${WINAPK_WIN_JDK:-}/lib/modules" ]]; then
        win_system="$WINAPK_WIN_JDK"
    fi
    : > "$WORK/$module.list"
    for src in "$@"; do
        [[ -d "$src" ]] || continue
        echo "[build_java] compiling ${src#"$RT"/} (--patch-module $module)..."
        # sun.nio.fs.Windows* patch the Windows JDK's own classes, which a
        # JDK for another system does not have. A Linux build compiles them
        # against a Windows JDK's image when WINAPK_WIN_JDK names one (as
        # test/run_app_wine.sh needs it anyway): without them java.nio does not
        # map Android paths, and Files.move on "/data/user/0/..." (androidx
        # DataStore's every save) finds no file. Without WINAPK_WIN_JDK they are
        # left out and the rest is checked.
        find "$src" -name '*.java' | { if command -v cygpath > /dev/null 2>&1 || [[ -n "$win_system" ]]; then cat; else
            grep -v '/sun/nio/fs/Windows'; fi; } | sort | native_paths >> "$WORK/$module.list"
        dirs="${dirs:+$dirs$sep}$(native_path "$src")"
    done
    [[ -s "$WORK/$module.list" ]] || return 0
    rm -rf "$dst"
    mkdir -p "$dst"
    "$JAVAC" -nowarn --source 21 --target 21 ${win_system:+--system "$win_system"} --patch-module "$module=$dirs" -d "$dst" @"$WORK/$module.list"
    [[ -n "$win_system" || "$module" != java.base ]] || command -v cygpath > /dev/null 2>&1 ||
        echo "[build_java] note: sun.nio.fs.Windows* left out (set WINAPK_WIN_JDK to a Windows JDK to build them)"
}
patch_module java.base "$LIB/java_base_patch" "$RT/java_base_patch" "$HERE/../java/java_base_patch"
patch_module jdk.unsupported "$LIB/jdk_unsupported_patch" "$RT/jdk_unsupported_patch"

echo "[build_java] packaging..."
( cd "$TMP" && find . -type f -name '*.class' ! -path './com/macapk/*' ! -path './com/android/apksig/*' | sort > "$WORK/framework.list" )
( cd "$TMP" && "$JAR" --create --file "$LIB/android-framework.jar" @"$WORK/framework.list" )
( cd "$TMP" && "$JAR" --create --file "$LIB/macapk-loader.jar" com/macapk com/android/apksig apksig-legal )
if [[ -d "$TMP/META-INF/services" ]]; then
    ( cd "$TMP" && "$JAR" --update --file "$LIB/macapk-loader.jar" META-INF/services )
fi
# bcprov, unsigned: its signature (an 800 KB manifest of per-entry digests and
# a .SF/.DSA pair) only costs. Class-data sharing skips every class of a signed
# jar, and the class loader parses the whole manifest when it defines the
# first BC package. OpenJDK does not check a JCE provider's signature
# (javax.crypto.ProviderVerifier is a no-op there), so nothing else changes.
# Only the manifest's main section is kept.
BC="$WORK/bcprov"
rm -rf "$BC"
mkdir -p "$BC/classes"
( cd "$BC/classes" && "$JAR" -xf "$(native_path "$RT/thirdparty/bcprov-jdk18on-1.79.jar")" )
awk '{ sub(/\r$/, "") } /^$/ { exit } { print }' "$BC/classes/META-INF/MANIFEST.MF" > "$BC/MANIFEST.MF"
rm -f "$BC/classes/META-INF/MANIFEST.MF" "$BC"/classes/META-INF/*.SF "$BC"/classes/META-INF/*.DSA \
    "$BC"/classes/META-INF/*.RSA "$BC"/classes/META-INF/*.EC
rm -f "$LIB/bcprov-jdk18on-1.79.jar"
"$JAR" --create --file "$LIB/bcprov-jdk18on-1.79.jar" --manifest "$BC/MANIFEST.MF" -C "$BC/classes" .

# Our own helpers, kept out of upstream's jars.
echo "[build_java] compiling windows/java..."
TOOLS="$WORK/tools"
rm -rf "$TOOLS"
mkdir -p "$TOOLS"
# java_base_patch is an overlay of java.base (compiled above), not a tool; aosp_gfx goes into
# aosp-graphics.jar (build_aosp_gfx.sh), first on every framework JVM's class path;
# aosp_res into aosp-resources.jar (build_aosp_res.sh) and aosp_views into aosp-views.jar
# (build_aosp_views.sh), always first: stage_runtime.sh then drops what they shadow from
# android-framework.jar; aosp_app into aosp-app.jar (build_aosp_app.sh, not staged yet).
find "$HERE/../java" -name '*.java' -not -path '*/java_base_patch/*' -not -path '*/aosp_gfx/*' \
    -not -path '*/aosp_res/*' -not -path '*/aosp_views/*' -not -path '*/aosp_app/*' | sort | native_paths > "$WORK/tools.list"
# AppMeta calls into the framework and the loader, compiled into $TMP above, and the graphics.
"$JAVAC" -d "$TOOLS" --release 21 -cp "$(native_path "$TMP")$CP_SEP$(native_path "$GFX_API")$CP_SEP$(native_path "$APP_API")" @"$WORK/tools.list"
( cd "$TOOLS" && "$JAR" --create --file "$LIB/winapk-tools.jar" . )

# The class list of the framework archive (host/cds.h), made from the smoke
# apps' starts by windows/scripts/merge_classlists.py.
if [[ -f "$HERE/../java/winapk-framework.classlist" ]]; then
    cp -f "$HERE/../java/winapk-framework.classlist" "$LIB/"
fi

echo "[build_java] $LIB/android-framework.jar"
echo "[build_java] $LIB/macapk-loader.jar"
echo "[build_java] $LIB/winapk-tools.jar"
