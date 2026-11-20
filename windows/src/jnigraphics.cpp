// jnigraphics.cpp -- see jnigraphics.h.
//
// The NDK's bitmap API lets native code read and write a Bitmap's pixels
// directly. Win.apk's android.graphics.Bitmap (runtime/java) is upstream's, so
// the implementation is upstream's as well: the libjnigraphics section of
// runtime/translator/src/bionic_shim.cpp, cut out at build time
// (cmake/extract_bitmap.cmake) and compiled unchanged. Its own comment has the
// details. In short, a Bitmap's pixels are a Java int[] of 0xAARRGGBB words
// whatever its Config, so getInfo reports RGBA_8888 with stride = width * 4;
// lockPixels hands the guest a buffer of its own in a device's byte order
// (R, G, B, A, premultiplied for a premultiplied bitmap), and unlockPixels
// merges the guest's writes back into the int[].
//
// What this file adds is the boundary: System V entry points, and the guest's
// JNIEnv swapped for the host one it wraps (jobject handles are HotSpot's on
// both sides). AndroidBitmapInfo has the same layout on both sides, so it is
// filled in place:
//
//   AndroidBitmapInfo   Android x86_64       Windows x64
//   width                0  uint32_t          0  uint32_t
//   height               4  uint32_t          4  uint32_t
//   stride               8  uint32_t          8  uint32_t
//   format              12  int32_t          12  int32_t
//   flags               16  uint32_t         16  uint32_t
//   sizeof              20                   20

#include "winapk/jnigraphics.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <jni.h>

#include "bitmap_upstream.inc"

#include "winapk/guest_abi.h"
#include "winapk/guest_jni.h"

namespace winapk {

namespace {

static_assert(sizeof(MacapkAndroidBitmapInfo) == 20 && offsetof(MacapkAndroidBitmapInfo, format) == 12 &&
                  offsetof(MacapkAndroidBitmapInfo, flags) == 16,
              "AndroidBitmapInfo (android/bitmap.h), as drawn above");

JNIEnv* host_env(JNIEnv* guest) { return guest ? host_env_of(guest) : nullptr; }

// ---- AOSP's Bitmap (the default graphics, host/aosp_gfx.h) ----
//
// Upstream's lock reads and writes the Java int[] behind Win.apk's own Bitmap (macapkArgbBuffer,
// macapkCommitArgbToAwt), which AOSP's Bitmap has not: its pixels are Skia's, in the native
// runtime. For it the lock takes the pixels into an int[] of its own with getPixels, runs
// upstream's conversion (the device's R, G, B, A bytes, premultiplied for a premultiplied bitmap)
// and merge over that array, and a sync that found a guest write puts the array back with
// setPixels. getInfo is upstream's as it is: it asks only what both Bitmaps answer.

bool is_aosp_bitmap(JNIEnv* env, jobject bmp) {
    jclass c = env->GetObjectClass(bmp);
    if (!c) {
        env->ExceptionClear();
        return false;
    }
    jmethodID m = env->GetMethodID(c, "macapkArgbBuffer", "()[I");
    env->DeleteLocalRef(c);
    if (m) return false;
    env->ExceptionClear();
    return true;
}

// Bitmap.getPixels / setPixels over the whole bitmap; false on any failure.
bool aosp_pixels(JNIEnv* env, jobject bmp, jintArray arr, int w, int h, bool write) {
    jclass c = env->GetObjectClass(bmp);
    if (!c) {
        env->ExceptionClear();
        return false;
    }
    jmethodID m = env->GetMethodID(c, write ? "setPixels" : "getPixels", "([IIIIIII)V");
    env->DeleteLocalRef(c);
    if (!m) {
        env->ExceptionClear();
        return false;
    }
    env->CallVoidMethod(bmp, m, arr, 0, w, 0, 0, w, h);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    return true;
}

// The sync of upstream's macapk_abm_sync: the array brought up to the bitmap first (Java's draws
// since the last sync), then merged with the guest's buffer; written back when the guest changed a
// pixel. -1 when the bitmap could not be read or written, else 1 / 0 as upstream's.
int aosp_sync(JNIEnv* env, jobject bmp, MacapkBitmapLock& lk, int w, int h) {
    if (lk.java_count == 0) return 0;
    if (!aosp_pixels(env, bmp, lk.array_ref, w, h, false)) return -1;
    void* java = env->GetPrimitiveArrayCritical(lk.array_ref, nullptr);
    if (!java) {
        env->ExceptionClear();
        return -1;
    }
    const bool changed =
        macapk_abm_merge(lk.pixels, static_cast<uint32_t*>(java), lk.seen, lk.java_count, lk.mode);
    env->ReleasePrimitiveArrayCritical(lk.array_ref, java, changed ? 0 : JNI_ABORT);
    if (changed && !aosp_pixels(env, bmp, lk.array_ref, w, h, true)) return -1;
    return changed ? 1 : 0;
}

int aosp_lock(JNIEnv* env, jobject bmp, void** addr) {
    MacapkAbmKeepPending keep(env);
    std::lock_guard<std::mutex> g(g_abm_mu);
    if (macapk_abm_bool_call(env, bmp, "isRecycled") != 0) return MACAPK_ABM_RESULT_JNI_EXCEPTION;
    const int w = macapk_abm_int_call(env, bmp, "getWidth");
    const int h = macapk_abm_int_call(env, bmp, "getHeight");
    if (w < 0 || h < 0) return MACAPK_ABM_RESULT_JNI_EXCEPTION;
    for (MacapkBitmapLock& lk : g_abm_locks) {
        if (env->IsSameObject(lk.bitmap_ref, bmp)) {
            if (aosp_sync(env, bmp, lk, w, h) < 0) return MACAPK_ABM_RESULT_JNI_EXCEPTION;
            ++lk.refs;
            if (addr) *addr = lk.pixels;
            return MACAPK_ABM_RESULT_SUCCESS;
        }
    }
    const int has_alpha = macapk_abm_bool_call(env, bmp, "hasAlpha");
    const int premul = macapk_abm_bool_call(env, bmp, "isPremultiplied");
    if (has_alpha < 0 || premul < 0) return MACAPK_ABM_RESULT_JNI_EXCEPTION;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    jintArray arr = env->NewIntArray(static_cast<jsize>(n));
    if (!arr) {
        env->ExceptionClear();
        return MACAPK_ABM_RESULT_JNI_EXCEPTION;
    }
    if (n && !aosp_pixels(env, bmp, arr, w, h, false)) {
        env->DeleteLocalRef(arr);
        return MACAPK_ABM_RESULT_JNI_EXCEPTION;
    }
    auto* px = static_cast<uint32_t*>(std::malloc((n ? n : 1) * sizeof(uint32_t)));
    auto* seen = static_cast<uint32_t*>(std::malloc((n ? n : 1) * sizeof(uint32_t)));
    void* java = (px && seen && n) ? env->GetPrimitiveArrayCritical(arr, nullptr) : nullptr;
    if (!px || !seen || (n && !java)) {
        env->ExceptionClear();
        std::free(px);
        std::free(seen);
        env->DeleteLocalRef(arr);
        return MACAPK_ABM_RESULT_JNI_EXCEPTION;
    }
    MacapkBitmapLock lk;
    lk.mode = macapk_abm_mode(has_alpha == 1, premul == 1);
    if (n) {
        macapk_abm_export(static_cast<const uint32_t*>(java), px, seen, n, lk.mode);
        env->ReleasePrimitiveArrayCritical(arr, java, JNI_ABORT);
    }
    lk.bitmap_ref = env->NewGlobalRef(bmp);
    lk.array_ref = static_cast<jintArray>(env->NewGlobalRef(arr));
    lk.pixels = px;
    lk.seen = seen;
    lk.java_count = n;
    lk.refs = 1;
    g_abm_locks.push_back(lk);
    env->DeleteLocalRef(arr);
    if (addr) *addr = px;
    return MACAPK_ABM_RESULT_SUCCESS;
}

int aosp_unlock(JNIEnv* env, jobject bmp) {
    MacapkAbmKeepPending keep(env);
    std::lock_guard<std::mutex> g(g_abm_mu);
    size_t i = 0;
    while (i < g_abm_locks.size() && !env->IsSameObject(g_abm_locks[i].bitmap_ref, bmp)) ++i;
    if (i == g_abm_locks.size()) return MACAPK_ABM_RESULT_JNI_EXCEPTION;
    MacapkBitmapLock& lk = g_abm_locks[i];
    int rc = MACAPK_ABM_RESULT_SUCCESS;
    if (macapk_abm_bool_call(env, bmp, "isRecycled") == 1) {
        rc = MACAPK_ABM_RESULT_JNI_EXCEPTION;  // bitmap.cpp: a recycled bitmap commits nothing
    } else {
        const int w = macapk_abm_int_call(env, bmp, "getWidth");
        const int h = macapk_abm_int_call(env, bmp, "getHeight");
        if (w < 0 || h < 0 || aosp_sync(env, bmp, lk, w, h) < 0) rc = MACAPK_ABM_RESULT_JNI_EXCEPTION;
    }
    if (--lk.refs == 0) {
        const MacapkBitmapLock done = lk;
        g_abm_locks.erase(g_abm_locks.begin() + static_cast<long>(i));
        env->DeleteGlobalRef(done.array_ref);
        env->DeleteGlobalRef(done.bitmap_ref);
        std::free(done.pixels);
        std::free(done.seen);
    }
    return rc;
}

WINAPK_GUEST_ABI int g_AndroidBitmap_getInfo(JNIEnv* env, jobject bitmap, MacapkAndroidBitmapInfo* info) {
    return __macapk_arm64_AndroidBitmap_getInfo(host_env(env), bitmap, info);
}
WINAPK_GUEST_ABI int g_AndroidBitmap_lockPixels(JNIEnv* env, jobject bitmap, void** pixels) {
    JNIEnv* host = host_env(env);
    if (host && bitmap && is_aosp_bitmap(host, bitmap)) return aosp_lock(host, bitmap, pixels);
    return __macapk_arm64_AndroidBitmap_lockPixels(host, bitmap, pixels);
}
WINAPK_GUEST_ABI int g_AndroidBitmap_unlockPixels(JNIEnv* env, jobject bitmap) {
    JNIEnv* host = host_env(env);
    if (host && bitmap && is_aosp_bitmap(host, bitmap)) return aosp_unlock(host, bitmap);
    return __macapk_arm64_AndroidBitmap_unlockPixels(host, bitmap);
}

}  // namespace

void* jnigraphics_resolve(const char* name) {
    if (std::strcmp(name, "AndroidBitmap_getInfo") == 0) return reinterpret_cast<void*>(&g_AndroidBitmap_getInfo);
    if (std::strcmp(name, "AndroidBitmap_lockPixels") == 0) {
        return reinterpret_cast<void*>(&g_AndroidBitmap_lockPixels);
    }
    if (std::strcmp(name, "AndroidBitmap_unlockPixels") == 0) {
        return reinterpret_cast<void*>(&g_AndroidBitmap_unlockPixels);
    }
    return nullptr;
}

}  // namespace winapk
