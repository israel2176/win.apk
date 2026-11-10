// paths_test.cpp -- winapk-paths-test: the guest path mapper (src/bionic/paths.cpp,
// winapk::android_host_path), which turns an Android path ("/data/user/0/<pkg>/x",
// "/storage/emulated/0/y") into the host file it names. This is the one table the
// Bionic shim, the framework's SQLite VFS, java.io and java.nio all go by, so a
// regression here moves every app's files.
//
// Exit code: the number of failed checks.

#include "winapk/mini_bionic.h"

#include <cstdio>
#include <string>

namespace {

int g_failed = 0;

// Compare with separators folded, since the mapper keeps the '/' of the matched
// tail ("<root>\\sdcard/Download") and Windows reads either slash.
std::wstring fold(std::wstring s) {
    for (wchar_t& c : s)
        if (c == L'/') c = L'\\';
    return s;
}

void check(const char* what, const std::wstring& got, const std::wstring& want) {
    if (fold(got) == fold(want)) return;
    ++g_failed;
    std::fprintf(stderr, "FAIL %s: got \"%ls\" want \"%ls\"\n", what, got.c_str(), want.c_str());
}

std::wstring host(const char* p) { return winapk::android_host_path(p); }

}  // namespace

int main() {
    const char* root = "C:\\root";
    const char* app_host = "C:\\root\\data\\data\\sub-8.3";  // the real folder, not named after the package
    winapk::set_android_root(root);
    winapk::set_app_data("com.example.app", app_host);

    // The app's own data directory, by both device spellings.
    check("data/user/0", host("/data/user/0/com.example.app/files/x.db"),
          L"C:\\root\\data\\data\\sub-8.3\\files\\x.db");
    check("data/data", host("/data/data/com.example.app/databases/y.db"),
          L"C:\\root\\data\\data\\sub-8.3\\databases\\y.db");
    check("data/user/0 root only", host("/data/user/0/com.example.app"),
          L"C:\\root\\data\\data\\sub-8.3");

    // Primary shared storage, by every device spelling.
    check("storage/emulated/0", host("/storage/emulated/0/Download/a.bin"),
          L"C:\\root\\sdcard\\Download\\a.bin");
    check("sdcard", host("/sdcard/DCIM/p.jpg"), L"C:\\root\\sdcard\\DCIM\\p.jpg");
    check("storage/self/primary", host("/storage/self/primary/m.txt"),
          L"C:\\root\\sdcard\\m.txt");
    check("mnt/sdcard", host("/mnt/sdcard/n.txt"), L"C:\\root\\sdcard\\n.txt");

    // Another package's data (no device would allow it): it is NOT the app's own
    // folder, so it rebases under the root as the generic rule, not app_data.
    check("other package data", host("/data/data/com.other/z"),
          L"C:\\root\\data\\data\\com.other\\z");
    // Device-protected storage sibling (StorageContext).
    check("data/user_de", host("/data/user_de/0/com.example.app/files"),
          L"C:\\root\\data\\user_de\\0\\com.example.app\\files");

    // /dev/null is Windows' NUL.
    check("dev/null", host("/dev/null"), L"NUL");

    // A file URL's path ("/C:/x", /D:/...) is the drive path, not an Android one;
    // the drive-letter scan strips the leading slash.
    check("file url drive path", host("/C:/a/b.jar"), L"C:/a/b.jar");
    check("file url drive path D", host("/D:/x/y"), L"D:/x/y");

    // A host path (drive letter, UNC, relative) passes through unchanged.
    check("host drive path", host("C:\\Users\\x\\file"), L"C:\\Users\\x\\file");
    check("host unc", host("\\\\server\\share\\f"), L"\\\\server\\share\\f");
    check("relative", host("files/sub"), L"files/sub");
    check("empty", host(""), L"");

    // SQLite's unixFullPathname concatenation ("<cwd>/C:\\x" or "/C:\\x"): the
    // last drive-letter path after a separator is the one meant.
    check("concatenated cwd + drive", host("C:\\cwd/C:\\data\\x.db"), L"C:\\data\\x.db");
    check("leading slash + drive", host("/C:\\data\\x.db"), L"C:\\data\\x.db");

    // A generic Android absolute path with no special prefix nests under the root.
    check("generic /opt", host("/opt/thing"), L"C:\\root\\opt\\thing");

    if (g_failed == 0) std::fprintf(stderr, "paths_test: all checks passed\n");
    return g_failed;
}
