// aosp_gfx.h -- WINAPK_AOSP_GFX=1: an app's run on AOSP's own graphics (windows/README.md,
// "Milestone 2: the runtime on AOSP graphics"). Off by default, and then nothing here does
// anything: the class path, the JVM's options and the natives are as they were.
//
// What the switch needs, all staged by windows/scripts/build_aosp_gfx.sh:
//   lib\aosp-graphics.jar               AOSP 14's android.graphics.**, the android.text classes that
//                                       measure and lay out, android.util.PathParser, + Win.apk's
//                                       stand-ins (windows/java/aosp_gfx); put before
//                                       android-framework.jar on the class path (first wins)
//   bin\robolectric-nativeruntime.dll   Robolectric's native runtime (Skia, minikin, HarfBuzz...)
//   bin\aosp\icu\icudt68l.dat           its ICU data
//   bin\aosp\fonts\fonts.xml + fonts    Android's fonts (-Dwinapk.aosp.fonts)
//   bin\aosp\hyphen-data\               the hyphenation patterns (-Dwinapk.aosp.hyphen)
// Any of them missing, or the DLL not loading in this process, and the run goes on with the
// switch off, saying so in the log.

#pragma once

#include <filesystem>
#include <string>

#include <jni.h>

namespace winapk::host {

// Decides, once, before the JVM is made: WINAPK_AOSP_GFX=1 and every file in place (the DLL is
// loaded here to find out). `lib` is the runtime's lib folder, `bin` winapk-runtime.exe's.
// Logs one line either way when the switch was asked for; silent when it was not.
bool aosp_gfx_prepare(const std::filesystem::path& lib, const std::filesystem::path& bin);

struct AospGfxFiles {
    std::filesystem::path jar, dll, icu_data, fonts, hyphen;
};
// The files, once aosp_gfx_prepare said yes; null when the switch is off. runtime_main.cpp's
// jvm_options puts the jar first on the class path and passes the rest as -D options.
const AospGfxFiles* aosp_gfx_files();
// The locale ICU starts in ("en-US"): the user's, as Windows has it.
std::string aosp_gfx_locale();

// After the host's own natives (host_natives.h) and SQLite's are bound: binds the DLL's natives
// onto aosp-graphics.jar's classes (aosp_gfx_redirect.h) and logs what it did. False when it could
// not; the app still starts, but AOSP's classes then have no natives.
bool aosp_gfx_load(JavaVM* vm, JNIEnv* env);

}  // namespace winapk::host
