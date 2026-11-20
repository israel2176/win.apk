// aosp_gfx_redirect.h -- binds Robolectric's native runtime (robolectric-nativeruntime.dll: AOSP's
// libhwui, Skia, minikin, HarfBuzz, FreeType, ICU) onto AOSP's own android.graphics / android.text
// classes (lib/aosp-graphics.jar) while the library loads, and onto nothing else.
//
// The library's JNI_OnLoad registers every native it has on Robolectric's
// "org/robolectric/nativeruntime/<X>Natives" classes. For as long as it loads, the JNI function
// table (JVMTI SetJNIFunctionTable, process-wide) is swapped for one whose
//   FindClass       maps such a name back to the AOSP class (loaded, not initialized), and the
//                   real names of the classes Win.apk keeps its own of (android.view.Surface,
//                   android.media.ImageReader, android.database(.sqlite), PropertyValuesHolder...)
//                   to a stand-in, so the library's field and method lookups on them find nothing
//                   of Win.apk's to hold on to;
//   RegisterNatives registers one method at a time, only on the allowlist (kAllowedPrefixes and
//                   kAllowedClasses in the .cpp:
//                   android.graphics.**, android.text.**, android.util.PathParser and the two
//                   helpers they free their native memory through), skipping a variant of another
//                   API level instead of failing the class, and adapting the four signatures API 34
//                   changed.
// Then the original table goes back. windows/test/aospgfx/redirect_agent.c is the test's own copy
// of the same redirect without the allowlist (it runs on android-all alone, where every class is
// AOSP's).
//
// Portable (jni.h, jvmti.h only), so it builds for a Linux JVM too.

#pragma once

#include <jni.h>
#include <jvmti.h>

namespace winapk::host::aosp_gfx {

struct RedirectCounts {
    int bound = 0;          // registered as the library has them
    int adapted = 0;        // registered through an API 34 signature adapter
    int skipped = 0;        // allowed, but the AOSP class has no such method (another API level's)
    int refused = 0;        // not on the allowlist: left to Win.apk's own natives
    int classes_unbound = 0;  // a *Natives class of the library's routed to the stand-in
    int faked = 0;          // lookups on Win.apk's classes that found nothing (logged each)
};

// Swaps the table in, for the calls of `env`'s thread (the one about to load the library; the
// table is process-wide, and any other thread's calls go through untouched). False (and nothing
// changed) when JVMTI refuses.
bool redirect_begin(jvmtiEnv* jvmti, JNIEnv* env);
// Puts the original table back.
bool redirect_end(jvmtiEnv* jvmti);
RedirectCounts redirect_counts();

// The allowlist, by binary name ("android.graphics.Canvas"). Exposed for the log and a test.
bool allowed_class(const char* binary_name);

}  // namespace winapk::host::aosp_gfx
