// blob_cache.cpp -- the EGL blob cache, as Android's libEGL keeps it for the GL driver.
//
// On a device libEGL hands the driver EGL_ANDROID_blob_cache functions over a file in the app's
// code cache (egl_set_cache_filename, from HardwareRenderer.setupDiskCache at bind), and the driver
// keeps its compiled programs there. ANGLE takes the same functions: its program binaries (each
// with the HLSL d3dcompiler_47 turned into Direct3D bytecode) are then read back on the next run
// instead of compiled again, which is most of the graphics' cost before the first frame. The file
// is libEGL's own format (FileBlobCache, frameworks/native/opengl/libs/EGL), written a moment after
// the last new entry, as Android's libEGL defers its write.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <windows.h>

#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "FileBlobCache.h"

namespace {

// Android libEGL's limits for the driver's cache (egl_cache.cpp): 12 KB keys, 64 KB values; the
// total larger (ANGLE's programs carry Direct3D bytecode and its own shader cache's entries).
constexpr size_t kMaxKey = 12 * 1024;
constexpr size_t kMaxValue = 2 * 1024 * 1024;
constexpr size_t kMaxTotal = 16 * 1024 * 1024;
constexpr DWORD kWriteDelayMs = 2000;

std::mutex g_lock;
std::string g_path;
std::unique_ptr<android::FileBlobCache> g_cache;
bool g_write_pending = false;

android::FileBlobCache* cache() {
    if (!g_cache && !g_path.empty()) g_cache = std::make_unique<android::FileBlobCache>(kMaxKey, kMaxValue, kMaxTotal, g_path);
    return g_cache.get();
}

void EGLAPIENTRY set_blob(const void* key, EGLsizeiANDROID key_size, const void* value, EGLsizeiANDROID value_size) {
    std::lock_guard<std::mutex> hold(g_lock);
    android::FileBlobCache* c = cache();
    if (!c || key_size <= 0 || value_size <= 0) return;
    c->set(key, static_cast<size_t>(key_size), value, static_cast<size_t>(value_size));
    if (g_write_pending) return;
    g_write_pending = true;
    std::thread([] {
        Sleep(kWriteDelayMs);
        std::lock_guard<std::mutex> hold(g_lock);
        g_write_pending = false;
        if (g_cache) g_cache->writeToFile();
    }).detach();
}

EGLsizeiANDROID EGLAPIENTRY get_blob(const void* key, EGLsizeiANDROID key_size, void* value,
                                     EGLsizeiANDROID value_size) {
    std::lock_guard<std::mutex> hold(g_lock);
    android::FileBlobCache* c = cache();
    if (!c || key_size <= 0) return 0;
    return static_cast<EGLsizeiANDROID>(
        c->get(key, static_cast<size_t>(key_size), value, value_size > 0 ? static_cast<size_t>(value_size) : 0));
}

}  // namespace

namespace android {
// HardwareRenderer.setupShadersDiskCache's (a host path: patch_src.py maps it). Before or after the
// display is up: the file is opened at the first blob asked for or kept.
void egl_set_cache_filename(const char* filename) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (g_cache) return;  // as libEGL: the first name stays
    g_path = filename ? filename : "";
}
}  // namespace android

namespace winapk::hwui {
void attach_blob_cache(EGLDisplay display) { eglSetBlobCacheFuncsANDROID(display, set_blob, get_blob); }
}  // namespace winapk::hwui
