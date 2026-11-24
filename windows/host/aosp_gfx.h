// aosp_gfx.h -- the runtime's JVMs (an app's run, an install, --gms, --verify-apk, --system) on AOSP's own
// graphics (windows/README.md, "Milestone 2: the runtime on AOSP graphics"), the only graphics there
// are: Win.apk's own Java2D classes are gone, so the framework cannot draw, measure text or decode a
// picture without these files.
//
// What it needs, all staged by windows/scripts/build_aosp_gfx.sh:
//   lib\aosp-graphics.jar               AOSP 14's android.graphics.**, the android.text classes that
//                                       measure and lay out, android.util.PathParser, + Win.apk's
//                                       stand-ins (windows/java/aosp_gfx); put before
//                                       android-framework.jar on the class path (first wins)
//   bin\robolectric-nativeruntime.dll   Robolectric's native runtime (Skia, minikin, HarfBuzz...)
//   bin\aosp\icu\icudt68l.dat           its ICU data
//   bin\aosp\fonts\fonts.xml + fonts    Android's fonts (-Dwinapk.aosp.fonts)
//   bin\aosp\hyphen-data\               the hyphenation patterns (-Dwinapk.aosp.hyphen)
// Any of them missing, or the DLL not loading in this process, and the process stops before it
// makes a JVM, with an "error: AOSP graphics" line in the log (the launcher's Show log): a stage or
// an install that is broken, not something a run can go on without.

#pragma once

#include <filesystem>
#include <string>

#include <jni.h>

namespace winapk::host {

// Checks, once, before the JVM is made, that every file is in place and that the DLL loads (it is
// loaded here to find out). `lib` is the runtime's lib folder, `bin` winapk-runtime.exe's. Logs one
// line either way; false means no JVM with the framework can be made (the caller stops).
bool aosp_gfx_prepare(const std::filesystem::path& lib, const std::filesystem::path& bin);

struct AospGfxFiles {
    std::filesystem::path jar, dll, icu_data, fonts, hyphen;
};
// The files, once aosp_gfx_prepare said yes; null before that or when it said no. runtime_main.cpp's
// jvm_options puts the jar first on the class path and passes the rest as -D options.
const AospGfxFiles* aosp_gfx_files();
// The locale ICU starts in ("en-US"): the user's, as Windows has it.
std::string aosp_gfx_locale();

// The UI gate greps a base commit for this line to know whether that base runs AOSP's resources
// and views by default (.github/workflows/ci.yml, "Find the base"): from this commit on there is
// no other way, as Win.apk's own views and resources are no longer shipped.
[[maybe_unused]] constexpr bool kAospViewsByDefault = true;

// AOSP's own resources (docs/research/aosp-views.md, step R1), always: Win.apk's own
// android.content.res is no longer in the shipped android-framework.jar (stage_runtime.sh drops
// the classes this jar and the views' shadow). windows/scripts/build_aosp_res.sh stages
//   lib\aosp-resources.jar           AOSP 14's android.content.res on Robolectric's Java port of
//                                    libandroidfw, + windows/java/aosp_res (its natives' bodies, the
//                                    ResBackend); put first on the class path, before the graphics'
//   bin\aosp\framework-res-34.apk    the framework's resources at the classes' level
//                                    (-Dwinapk.aosp.res.framework), the only platform table shipped
// They run with AOSP's graphics only (AOSP's Resources inflates AOSP's drawables). With those off,
// or a file missing, this logs that an app cannot start (a broken stage) and adds nothing. Pure
// Java: nothing to load here. WINAPK_AOSP_RES, the switch while they were new, is ignored.
struct AospResFiles {
    std::filesystem::path jar, framework;
};
// Decides, once, after aosp_gfx_prepare, for the same JVMs; logs one line.
bool aosp_res_prepare(const std::filesystem::path& lib, const std::filesystem::path& bin);
// The files, once aosp_res_prepare said yes; null otherwise.
const AospResFiles* aosp_res_files();

// AOSP's own views (docs/research/aosp-views.md, steps V1 and V2), always, likewise.
// windows/scripts/build_aosp_views.sh stages lib\aosp-views.jar (AOSP 14's android.view,
// android.widget, com.android.internal.{view,widget,policy} + windows/java/aosp_views: the
// ViewHost, the in-process window manager), put first on the class path, before the resources'.
// They run with AOSP's resources only (and so with its graphics). WINAPK_AOSP_VIEWS is ignored.
// Decides, once, after aosp_res_prepare, for the same JVMs; logs one line.
bool aosp_views_prepare(const std::filesystem::path& lib);
// The jar, once aosp_views_prepare said yes; null otherwise.
const std::filesystem::path* aosp_views_jar();

// AOSP's own app layer (docs/research/aosp-app.md): windows/scripts/build_aosp_app.sh stages
// lib\aosp-app.jar (AOSP 14's ActivityThread, client transactions, Instrumentation, ContextImpl,
// LoadedApk, Application, Activity + windows/java/aosp_app: the in-process activity manager and task
// manager), put first on the class path, before the views'. It runs on AOSP's views only.
// Decides, once, after aosp_views_prepare, for the same JVMs; logs one line.
bool aosp_app_prepare(const std::filesystem::path& lib);
// The jar, once aosp_app_prepare said yes; null otherwise.
const std::filesystem::path* aosp_app_jar();

// After the host's own natives (host_natives.h) and SQLite's are bound: binds the DLL's natives
// onto aosp-graphics.jar's classes (aosp_gfx_redirect.h) and logs what it did. False when it could
// not: AOSP's classes then have no natives, so the caller stops instead of running a JVM that
// cannot draw.
bool aosp_gfx_load(JavaVM* vm, JNIEnv* env);

}  // namespace winapk::host
