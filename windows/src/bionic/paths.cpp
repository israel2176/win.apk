// paths.cpp -- where a guest path lives on the host.
//
// Guest code names files two ways: with the host paths Java hands it
// (getFilesDir(), nativeLibraryDir: "C:\...\android\data\...") and with
// Android's own absolute paths, hard-coded or built by hand ("/data/data/<pkg>/
// files", "/sdcard/...", "/system/fonts/Roboto-Regular.ttf"). On Windows a path
// that starts with '/' means the root of the current drive, which is never what
// the guest means. Upstream's shim (macapk_arm64_remap_path in
// runtime/translator/src/bionic_shim.cpp) solves the same problem the same way:
//
//   /system/etc/fonts.xml, /system/fonts/...  -> the fonts directory Boot wrote
//                                                (setSystemFontsDir)
//   /system/etc/security/cacerts,
//   /apex/com.android.conscrypt/cacerts       -> the CA store cacerts.cpp builds
//                                                from the roots Windows trusts
//   /dev/null                                 -> NUL
//   any other /...                            -> <android root>/...
//
// The android root is the per-user folder the runtime mirrors Android's
// layout in (data/app, data/data/<pkg>, sdcard); the runtime sets it before
// any guest code runs.

#include "bionic_internal.h"

#include <cstring>
#include <mutex>
#include <string>

#include <windows.h>

#include "winapk/mini_bionic.h"

namespace winapk::bionic {

namespace {

std::mutex g_lock;
std::wstring g_android_root;  // no trailing separator
std::wstring g_fonts_dir;
std::string g_app_package;     // the running app's package, for /data/user/0/<package>
std::wstring g_app_data_dir;   // its data directory on the host (set_app_data)

std::wstring to_wide(const char* utf8, size_t len) {
    if (len == 0) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(len), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(len), &w[0], n);
    return w;
}

std::wstring trimmed(std::wstring w) {
    while (w.size() > 3 && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
    return w;
}

bool starts_with(const char* s, const char* prefix) { return std::strncmp(s, prefix, std::strlen(prefix)) == 0; }

// What follows Android's CA store directory in `p` ("" or "/<name>"), or null
// when `p` is not in it.
const char* cacerts_rest(const char* p) {
    for (const char* dir : {"/system/etc/security/cacerts", "/apex/com.android.conscrypt/cacerts"}) {
        const size_t n = std::strlen(dir);
        if (std::strncmp(p, dir, n) == 0 && (p[n] == 0 || p[n] == '/')) return p + n;
    }
    return nullptr;
}

}  // namespace

// The device's other spellings of two directories: the app's own data
// directory (/data/user/0/<package> and /data/data/<package>, which here is
// <android root>/data/data/<the app's folder>, not always named after the
// package), and primary shared storage (/storage/emulated/0, /sdcard and
// their aliases, all <android root>/sdcard). The rest of `p` after the match,
// or null. Caller holds g_lock.
const char* app_data_rest(const char* p) {
    if (g_app_package.empty() || g_app_data_dir.empty()) return nullptr;
    for (const char* dir : {"/data/user/0/", "/data/data/"}) {
        const size_t n = std::strlen(dir);
        if (std::strncmp(p, dir, n) != 0) continue;
        const char* pkg = p + n;
        const size_t m = g_app_package.size();
        if (std::strncmp(pkg, g_app_package.c_str(), m) == 0 && (pkg[m] == 0 || pkg[m] == '/')) return pkg + m;
    }
    return nullptr;
}
const char* storage_rest(const char* p) {
    for (const char* dir : {"/storage/emulated/0", "/storage/self/primary", "/mnt/sdcard", "/sdcard"}) {
        const size_t n = std::strlen(dir);
        if (std::strncmp(p, dir, n) == 0 && (p[n] == 0 || p[n] == '/')) return p + n;
    }
    return nullptr;
}

std::wstring host_path(const char* p) {
    if (!p) return {};
    size_t len = std::strlen(p);
    // Guest code that knows no drive letters takes the host paths Java hands
    // it ("C:\...") for relative ones and joins them onto getcwd() or "/":
    // SQLite's unixFullPathname yields "C:\cwd/C:\data\x.db" or "/C:\data\x.db".
    // The last drive-letter path after a separator is the one that was meant.
    for (size_t i = len; i-- > 2;) {
        if (p[i] == ':' && i + 1 < len && (p[i + 1] == '\\' || p[i + 1] == '/') &&
            ((p[i - 1] >= 'A' && p[i - 1] <= 'Z') || (p[i - 1] >= 'a' && p[i - 1] <= 'z')) &&
            (p[i - 2] == '/' || p[i - 2] == '\\')) {
            p += i - 1;
            len -= i - 1;
            break;
        }
    }
    // A host path (drive letter, UNC, relative): as it is.
    if (p[0] != '/' || p[1] == '/') return to_wide(p, len);
    if (std::strcmp(p, "/dev/null") == 0) return L"NUL";
    if (const char* rest = cacerts_rest(p)) {
        std::wstring root;
        {
            std::lock_guard<std::mutex> hold(g_lock);
            root = g_android_root;
        }
        const std::wstring dir = android_cacerts_dir(root);
        if (!dir.empty()) return dir + to_wide(rest, std::strlen(rest));
    }

    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_fonts_dir.empty()) {
        if (std::strcmp(p, "/system/etc/fonts.xml") == 0) return g_fonts_dir + L"\\fonts.xml";
        if (starts_with(p, "/system/fonts") && (p[13] == '\0' || p[13] == '/')) {
            return g_fonts_dir + to_wide(p + 13, len - 13);
        }
    }
    if (g_android_root.empty()) return to_wide(p, len);
    if (const char* rest = app_data_rest(p)) return g_app_data_dir + to_wide(rest, std::strlen(rest));
    if (const char* rest = storage_rest(p)) return g_android_root + L"\\sdcard" + to_wide(rest, std::strlen(rest));
    if (std::strncmp(p, "/data/user/0", 12) == 0 && (p[12] == 0 || p[12] == '/')) {
        return g_android_root + L"\\data\\data" + to_wide(p + 12, len - 12);
    }
    return g_android_root + to_wide(p, len);
}

}  // namespace winapk::bionic

namespace winapk {

void set_android_root(const char* dir) {
    std::lock_guard<std::mutex> hold(bionic::g_lock);
    bionic::g_android_root = bionic::trimmed(bionic::to_wide(dir, dir ? std::strlen(dir) : 0));
}
void set_app_data(const char* package, const char* host_dir) {
    std::lock_guard<std::mutex> hold(bionic::g_lock);
    bionic::g_app_package = package ? package : "";
    bionic::g_app_data_dir = bionic::trimmed(bionic::to_wide(host_dir, host_dir ? std::strlen(host_dir) : 0));
}
std::wstring android_host_path(const char* path) { return bionic::host_path(path); }
void set_system_fonts_dir(const char* dir) {
    std::lock_guard<std::mutex> hold(bionic::g_lock);
    bionic::g_fonts_dir = bionic::trimmed(bionic::to_wide(dir, dir ? std::strlen(dir) : 0));
}

}  // namespace winapk
