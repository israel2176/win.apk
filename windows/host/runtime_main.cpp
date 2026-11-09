// runtime_main.cpp -- winapk-runtime, the Windows host that runs an APK.
//
// The Windows counterpart of runtime/src/main.cpp, in the same order:
//
//   1. Find the runtime files: <root>/lib holds the jars that
//      scripts/build_java.sh builds, <root>/jre (or JAVA_HOME) the JDK.
//   2. Convert the APK's DEX to <app dir>/classes.jar with upstream's
//      converter (com.macapk.dex.Main), in a child java.exe, unless the cached
//      jar was made from this APK by this converter (.macapk-dex-stamp).
//   3. Create the JVM in-process with upstream's options, on a thread with a
//      large stack (-Xss does not apply to the thread that creates the VM).
//   4. Bind the host natives (host_natives.h).
//   5. Call com.macapk.loader.Boot.run.
//
//   winapk-runtime --apk <file.apk> [--data <dir>] [--jdk <dir>] [--lib <dir>] [--install-only]
//   winapk-runtime --apk <app> --package-to <file>   (the launcher's install helper: the package name)
//   winapk-runtime --verify-apk <app>   (the launcher's Google services setup: package and signer, below)
//   winapk-runtime --java-main --jdk <dir> --cp <jar> <class> [args...]   (internal: run_java)
//   winapk-runtime [--data <dir>] [--jdk <dir>] [--lib <dir>] --system   (the system broker, below)
//   winapk-runtime [--data <dir>] [--jdk <dir>] [--lib <dir>] --cds-prepare   (the framework archive now, cds.h)
//   winapk-runtime --cds-dump ...   (internal: the framework archive's background dump)
//
// --system runs upstream's system broker (com.macapk.loader.SystemServer: how
// one app binds another's services, the real Google Play services among them,
// when Settings > Google account > "Use Google services" is on). It is started
// by the first app that needs it (com.macapk.loader.WindowsBroker), is alone per
// user session (the Local\WinApk-system mutex, then its pipe name), and exits
// when no app has been open for a while.
//
// One runtime runs an app at a time, as on the Mac, where the app's stub holds
// a lock and a second launch brings the running app forward: a second start
// of the same app shows the running one's window and exits 0, and a second
// --install-only (an update while the app runs) exits 3 and changes nothing.
//
// --install-only stops after step 2 plus the launcher's metadata: the app's
// icon and name in <app dir>/icon.png and label.txt (com.winapk.tools.AppMeta,
// in a JVM with the host natives but no app running), which is what the
// launcher (launcher/) runs when an APK is added to the library.
//
// App data lives under %LOCALAPPDATA%\Win.apk unless --data says otherwise.
// The app's window is win32_window.h's; with WINAPK_HEADLESS=1 there is none
// and what the app does shows in the log.
//
// Paths may hold any character, whatever "Language for non-Unicode programs"
// is set to (a user name in Hebrew on an English system, say). The program
// uses the wide Windows calls for its own files, and its manifest
// (runtime.manifest) sets the process's ANSI code page to UTF-8 on Windows 10
// 1903 and later, which is how the JVM, which reads paths in that code page,
// gets them whole. Older systems get the paths in a form the code page can
// hold (jvm_path). The steps that run in a child JVM run in a child of this
// program (--java-main), not in java.exe, which reads its own location and
// its arguments in the ANSI code page.

#include "aot_guard.h"
#include "cds.h"
#include "crash_report.h"
#include "guest_libs.h"
#include "host_natives.h"
#include "user_folders.h"
#include "win32_window.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <jni.h>
#include <jvmti.h>
#include <windows.h>
#include <shellapi.h>

#include "winapk/aot_files.h"
#include "winapk/aot_stamp.h"
#include "winapk/arm64_aot.h"
#include "winapk/mini_bionic.h"

namespace macapk::sqlite {
void RegisterNativeSqliteMethods(JNIEnv* env);  // runtime/src/native_sqlite.cpp
}

namespace fs = std::filesystem;

namespace {

struct Options {
    fs::path apk, data, jdk, lib;
    bool install_only = false;
    bool default_data = false;  // no --data: the user's own library, not a test's scratch folder
    fs::path package_to;  // --package-to: write the app's package name there and stop
    bool gms = false;     // --gms <subcommand> [args]: com.macapk.gms.GmsCli, no app
    std::vector<std::wstring> gms_args;
    bool system = false;  // --system: the system broker, no app
    bool cds_prepare = false;  // --cds-prepare: make the framework archive (cds.h) and stop
    bool verify = false;  // --verify-apk <app>: com.winapk.tools.ApkCheck on `apk`, no app run
};

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring env_var(const wchar_t* name) {
    wchar_t buf[32768];
    const DWORD n = GetEnvironmentVariableW(name, buf, 32768);
    return (n > 0 && n < 32768) ? std::wstring(buf, n) : std::wstring();
}

// HKCU\Software\Win.apk GoogleServices (absent = 0): the launcher's switch
// Settings > Google account > "Use Google services (experimental)".
bool google_services_setting() {
    DWORD value = 0, size = sizeof value;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Win.apk", L"GoogleServices", RRF_RT_REG_DWORD, nullptr,
                        &value, &size) == ERROR_SUCCESS &&
           value != 0;
}

fs::path exe_dir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    return fs::path(std::wstring(buf, n)).parent_path();
}

// `w` in the process's ANSI code page, when every character of it survives
// the round trip. Under the manifest's UTF-8 code page that is always so.
bool to_ansi(const std::wstring& w, std::string* out) {
    if (GetACP() == CP_UTF8) {
        *out = utf8(w);
        return true;
    }
    out->clear();
    if (w.empty()) return true;
    BOOL lossy = FALSE;
    const int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()), nullptr,
                                      0, nullptr, &lossy);
    if (n <= 0 || lossy) return false;
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr,
                        &lossy);
    if (lossy) return false;
    *out = std::move(s);
    return true;
}

// A form of the existing path `p` that the ANSI code page can hold: `p`
// itself when it can, else its 8.3 short form (plain ASCII; NTFS has it
// unless the volume turned short names off). `p` when neither works, with
// *ok false.
fs::path ansi_safe(const fs::path& p, bool* ok = nullptr) {
    std::string bytes;
    if (ok) *ok = true;
    if (to_ansi(p.wstring(), &bytes)) return p;
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetShortPathNameW(p.c_str(), buf, MAX_PATH * 4);
    if (n > 0 && n < MAX_PATH * 4 && to_ansi(std::wstring(buf, n), &bytes)) return fs::path(std::wstring(buf, n));
    if (ok) *ok = false;
    return p;
}

// A path for a JVM option: HotSpot reads option strings, and opens the files
// they name, in the ANSI code page.
std::string jvm_path(const fs::path& p) {
    bool ok = true;
    const fs::path safe = ansi_safe(p, &ok);
    std::string bytes;
    if (ok && to_ansi(safe.wstring(), &bytes)) return bytes;
    std::fprintf(stderr,
                 "[win.apk] warning: %s has characters the ANSI code page (%u) cannot hold and no short name; "
                 "the JVM may not find it. Windows 10 1903 or later, or a folder with plain letters, avoids this.\n",
                 utf8(p.wstring()).c_str(), GetACP());
    return utf8(p.wstring());
}

// One argument for a command line, quoted the way CommandLineToArgvW and the
// C runtime split it again.
std::wstring quoted(const std::wstring& a) {
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        q.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        q += c;
    }
    q.append(slashes * 2, L'\\');
    return q + L"\"";
}
std::wstring quoted(const fs::path& p) { return quoted(p.wstring()); }

// Loads the JDK's jvm.dll. HotSpot finds java.dll and the rest of the JDK from
// jvm.dll's own path, which it reads in the ANSI code page, so the DLL is
// loaded by a path that code page can hold.
HMODULE load_jvm(const fs::path& jdk) {
    const fs::path home = ansi_safe(jdk);
    // jvm.dll needs the DLLs next to java.exe (the VC runtime among them).
    SetDllDirectoryW((home / "bin").c_str());
    HMODULE jvm = LoadLibraryW((home / "bin" / "server" / "jvm.dll").c_str());
    if (!jvm) std::fprintf(stderr, "[win.apk] cannot load jvm.dll (error %lu)\n", GetLastError());
    return jvm;
}

using CreateVmFn = jint(JNICALL*)(JavaVM**, void**, void*);

// HotSpot's fatal error handler (hs_err) calls it on its way out: a fault in
// native code called from Java ends there, as HotSpot's threads catch their
// own exceptions and the unhandled-exception filter never sees it.
void JNICALL jvm_abort_hook() { winapk::host::crash_report_fatal("the JVM's fatal error handler"); }

bool create_vm(HMODULE jvm, std::vector<std::string>& opts, JavaVM** vm, JNIEnv** env) {
    auto create = reinterpret_cast<CreateVmFn>(GetProcAddress(jvm, "JNI_CreateJavaVM"));
    if (!create) return false;
    std::vector<JavaVMOption> jopts(opts.size());
    for (size_t i = 0; i < opts.size(); ++i) {
        jopts[i].optionString = &opts[i][0];
        jopts[i].extraInfo = nullptr;
    }
    static char abort_option[] = "abort";
    jopts.push_back({abort_option, reinterpret_cast<void*>(&jvm_abort_hook)});
    JavaVMInitArgs args{};
    args.version = JNI_VERSION_21;
    args.nOptions = static_cast<jint>(jopts.size());
    args.options = jopts.data();
    args.ignoreUnrecognized = JNI_FALSE;
    if (create(vm, reinterpret_cast<void**>(env), &args) != JNI_OK) {
        std::fprintf(stderr, "[win.apk] JNI_CreateJavaVM failed\n");
        return false;
    }
    winapk::host::rearm_crash_report();  // HotSpot installed its own unhandled-exception filter
    winapk::host::cds_vm_created(*vm, *env);
    return true;
}

// A Java string straight from UTF-16, so any character survives.
jstring jstring_of(JNIEnv* env, const std::wstring& w) {
    return env->NewString(reinterpret_cast<const jchar*>(w.data()), static_cast<jsize>(w.size()));
}

bool parse_args(int argc, wchar_t** argv, Options* o) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto value = [&](fs::path* out) {
            if (i + 1 >= argc) return false;
            *out = fs::path(argv[++i]);
            return true;
        };
        bool ok = true;
        if (a == L"--apk") ok = value(&o->apk);
        else if (a == L"--data") ok = value(&o->data);
        else if (a == L"--jdk") ok = value(&o->jdk);
        else if (a == L"--lib") ok = value(&o->lib);
        else if (a == L"--install-only") o->install_only = true;
        else if (a == L"--package-to") ok = value(&o->package_to);
        else if (a == L"--system") o->system = true;
        else if (a == L"--cds-prepare") o->cds_prepare = true;
        else if (a == L"--verify-apk") {
            o->verify = true;
            ok = value(&o->apk);
        }
        else if (a == L"--gms") {
            // Everything after --gms belongs to the subcommand, verbatim, as
            // upstream's CliArgs::gms_args.
            o->gms = true;
            for (int j = i + 1; j < argc; ++j) o->gms_args.emplace_back(argv[j]);
            break;
        } else ok = false;
        if (!ok) return false;
    }
    return !o->apk.empty() || o->gms || o->system || o->cds_prepare;
}

// Fills in whatever the command line left out, and checks it exists.
bool resolve_paths(Options* o) {
    std::error_code ec;
    if (!o->gms && !o->system && !o->cds_prepare) {
        o->apk = fs::absolute(o->apk, ec);
        if (!fs::is_regular_file(o->apk, ec)) {
            std::fprintf(stderr, "[win.apk] no such APK: %s\n", utf8(o->apk.wstring()).c_str());
            return false;
        }
    }
    const fs::path root = exe_dir().parent_path();
    if (o->lib.empty()) o->lib = root / "lib";
    if (!fs::is_regular_file(o->lib / "android-framework.jar", ec)) {
        std::fprintf(stderr, "[win.apk] android-framework.jar not found in %s (use --lib)\n",
                     utf8(o->lib.wstring()).c_str());
        return false;
    }
    if (o->jdk.empty()) {
        if (!env_var(L"WINAPK_JAVA_HOME").empty()) o->jdk = env_var(L"WINAPK_JAVA_HOME");
        else if (fs::exists(root / "jre" / "bin" / "server" / "jvm.dll", ec)) o->jdk = root / "jre";
        else o->jdk = env_var(L"JAVA_HOME");
    }
    if (!fs::exists(o->jdk / "bin" / "server" / "jvm.dll", ec)) {
        std::fprintf(stderr, "[win.apk] no JDK with bin\\server\\jvm.dll (use --jdk or JAVA_HOME)\n");
        return false;
    }
    if (o->data.empty()) {
        const std::wstring local = env_var(L"LOCALAPPDATA");
        if (local.empty()) {
            std::fprintf(stderr, "[win.apk] LOCALAPPDATA is not set (use --data)\n");
            return false;
        }
        o->data = fs::path(local) / L"Win.apk";
        o->default_data = true;
    }
    return true;
}

// Runs `main_class` from `jar` with `args` in a child JVM, as
// `java -cp <jar> <main_class> <args>` would, and returns its exit code, or -1
// if it did not start. The child is this program (--java-main), not the JDK's
// java.exe: java.exe finds java.dll from its own path and parses its
// arguments in the ANSI code page, so under a folder with characters outside
// it it stopped at "could not find java.dll", and an argument such as the
// data folder reached Java as "?". The child gets the arguments as UTF-16
// Java strings, and its own default heap, as java.exe did: the DEX converter
// needs more than the app's 1 GiB for large apps.
int run_java(const Options& o, const fs::path& jar, const wchar_t* main_class, const std::vector<std::wstring>& args) {
    wchar_t self[32768];
    const DWORD n = GetModuleFileNameW(nullptr, self, 32768);
    if (n == 0 || n >= 32768) return -1;
    std::wstring cmdline = quoted(std::wstring(self, n)) + L" --java-main --jdk " + quoted(o.jdk) + L" --cp " +
                           quoted(jar) + L" " + main_class;
    for (const std::wstring& a : args) cmdline += L" " + quoted(a);
    std::fflush(stderr);  // our lines first, then the child's
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(self, &cmdline[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

// Runs a command line (a tool that is not Java: winapk-aot, clang) and returns
// its exit code, or -1 if it did not start.
int run_process(std::wstring cmdline) {
    std::fflush(stderr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

// ---- which converter made the cached code -----------------------------------
//
// <app dir>\.macapk-dex-stamp records what classes.jar was made from:
//
//   converter=<converter_id>   the converter (com/macapk/dex/ in macapk-loader.jar)
//   apk=<file_id>              the installed base.apk
//
// The jar is converted again when either line differs, and when there is no
// stamp (a jar from before the stamp, made by a converter nobody recorded,
// perhaps the very one an update fixed), so a converter fix reaches every
// app that was already converted. As upstream's CONVERTER_ID
// (runtime/scripts/build_ids.py), the id hashes the converter's entries'
// names, lengths and CRC-32s from the jar's central directory: a rebuild of
// the same sources, or a change to the framework or the loader only, keeps
// the jars. The name starts with ".macapk", so Boot's code-dir scan leaves
// the file here.

// "<size>:<last write time>" of a file, or "" when it cannot be read.
std::string file_id(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return {};
    const uint64_t size = static_cast<uint64_t>(a.nFileSizeHigh) << 32 | a.nFileSizeLow;
    const uint64_t time =
        static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32 | a.ftLastWriteTime.dwLowDateTime;
    return std::to_string(size) + ":" + std::to_string(time);
}

uint32_t le16(const unsigned char* p) { return p[0] | p[1] << 8; }
uint32_t le32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

// "<name> <length> <CRC-32>" of each file entry of `jar` under `prefix`, from
// the central directory (nothing is decompressed). False when the jar cannot
// be read that way (not a zip, or ZIP64).
bool jar_entries(const fs::path& jar, const std::string& prefix, std::vector<std::string>* rows) {
    FILE* f = _wfopen(jar.c_str(), L"rb");
    if (!f) return false;
    auto read_at = [f](long long at, size_t n, std::vector<unsigned char>* out) {
        out->resize(n);
        return _fseeki64(f, at, SEEK_SET) == 0 && std::fread(out->data(), 1, n, f) == n;
    };
    bool ok = false;
    const long long size = _fseeki64(f, 0, SEEK_END) == 0 ? _ftelli64(f) : -1;
    // The end of central directory record: 22 bytes, then up to 64 KB of comment.
    const long long tail = std::min<long long>(size, 22 + 0xFFFF);
    std::vector<unsigned char> end, dir;
    if (tail >= 22 && read_at(size - tail, static_cast<size_t>(tail), &end)) {
        for (size_t i = end.size() - 22 + 1; i-- > 0;) {
            if (le32(&end[i]) != 0x06054b50) continue;
            const uint32_t dir_size = le32(&end[i + 12]), dir_at = le32(&end[i + 16]);
            if (static_cast<long long>(dir_at) + dir_size > size - tail + static_cast<long long>(i) ||
                !read_at(dir_at, dir_size, &dir)) {
                break;
            }
            size_t p = 0;
            while (p + 46 <= dir.size() && le32(&dir[p]) == 0x02014b50) {
                const size_t name_len = le16(&dir[p + 28]);
                const size_t next = p + 46 + name_len + le16(&dir[p + 30]) + le16(&dir[p + 32]);
                if (next > dir.size()) break;
                const std::string name(reinterpret_cast<const char*>(&dir[p + 46]), name_len);
                if (name.rfind(prefix, 0) == 0 && name.back() != '/') {
                    rows->push_back(name + ' ' + std::to_string(le32(&dir[p + 24])) + ' ' +
                                    std::to_string(le32(&dir[p + 16])));
                }
                p = next;
            }
            ok = p == dir.size();
            break;
        }
    }
    std::fclose(f);
    return ok;
}

// "dex:" and an FNV-1a hash of the converter's entries, or "jar:<file_id>"
// when the jar cannot be read so.
std::string converter_id(const fs::path& loader_jar) {
    std::vector<std::string> rows;
    if (!jar_entries(loader_jar, "com/macapk/dex/", &rows) || rows.empty()) return "jar:" + file_id(loader_jar);
    std::sort(rows.begin(), rows.end());
    uint64_t h = 1469598103934665603ull;
    for (const std::string& r : rows) {
        for (const char c : r + '\n') h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    }
    char id[24];
    std::snprintf(id, sizeof id, "dex:%016llx", static_cast<unsigned long long>(h));
    return id;
}

struct DexStamp {
    std::string converter, apk;
};

bool read_dex_stamp(const fs::path& path, DexStamp* s) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::string text;
    char buf[512];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    std::fclose(f);
    for (size_t i = 0; i < text.size();) {
        const size_t j = std::min(text.find('\n', i), text.size());
        const std::string line = text.substr(i, j - i);
        if (line.rfind("converter=", 0) == 0) s->converter = line.substr(10);
        else if (line.rfind("apk=", 0) == 0) s->apk = line.substr(4);
        i = j + 1;
    }
    return !s->converter.empty() && !s->apk.empty();
}

// To a temporary name, then renamed: the stamp is whole or absent.
bool write_dex_stamp(const fs::path& path, const DexStamp& s) {
    std::error_code ec;
    const fs::path tmp = path.wstring() + L".tmp";
    const std::string text = "converter=" + s.converter + "\napk=" + s.apk + "\n";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    const bool written = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) != 0 || !written) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

// Step 2. Exit codes are com.macapk.dex.Main's: 0 converted, 3 no DEX in the
// APK (a valid case: the app then has no classes.jar). A failed conversion
// keeps the cached jar when only the converter changed: the app then runs as
// before the update, and the next start converts again.
bool convert_dex(const Options& o, const fs::path& app_dir, fs::path* jar) {
    std::error_code ec;
    *jar = app_dir / "classes.jar";
    const fs::path stamp = app_dir / ".macapk-dex-stamp";
    const DexStamp want{converter_id(o.lib / "macapk-loader.jar"), file_id(o.apk)};
    DexStamp have;
    const bool stamped = read_dex_stamp(stamp, &have);
    const bool cached = fs::exists(*jar, ec);
    const char* why = !cached                            ? "once per install"
                      : !stamped                         ? "the cached copy has no converter stamp"
                      : have.converter != want.converter ? "the converter changed"
                      : have.apk != want.apk             ? "the app changed"
                                                         : nullptr;
    if (!why) return true;
    std::fprintf(stderr, "[win.apk] converting the app's code (%s)...\n", why);
    const fs::path tmp = app_dir / "classes.jar.tmp";
    const int rc = run_java(o, o.lib / "macapk-loader.jar", L"com.macapk.dex.Main", {o.apk.wstring(), tmp.wstring()});
    if (rc == 3) {
        jar->clear();
        return true;
    }
    if (rc == 0) {
        fs::rename(tmp, *jar, ec);
        if (!ec) {
            if (!write_dex_stamp(stamp, want)) {
                std::fprintf(stderr, "[win.apk] warning: cannot write %s; the next start converts the code again\n",
                             utf8(stamp.wstring()).c_str());
            }
            return true;
        }
        std::fprintf(stderr, "[win.apk] DEX conversion failed: cannot replace %s (%s)\n", utf8(jar->wstring()).c_str(),
                     ec.message().c_str());
        fs::remove(tmp, ec);
    } else {
        std::fprintf(stderr, "[win.apk] DEX conversion failed (exit code %d)\n", rc);
    }
    const bool same_apk = cached && (stamped ? have.apk == want.apk
                                             : fs::last_write_time(*jar, ec) >= fs::last_write_time(o.apk, ec));
    if (!same_apk) return false;
    std::fprintf(stderr, "[win.apk] WARNING: the app's code was NOT converted again; it runs on the previous "
                         "converter's copy, and the next start converts again\n");
    return true;
}

// Step 1: install the app the way a device's installer does, into
// <android root>\data\app\~~<nonce>\<package>-<nonce>: base.apk (a hard link
// to a plain APK, or unpacked from a bundle) plus split_*.apk, and the native
// libraries of this host's ABI under lib\<isa>. Upstream's runtime and its DEX
// converter recognize an installed app only in that layout
// (com.winapk.tools.AppInstall). Done once per input file; a small record next
// to the installs names the result. Afterwards the app is the installed
// base.apk.
//
// WINAPK_ABIS overrides the ABI list (WINAPK_ABIS=arm64-v8a runs an app's
// arm64 build although it has an x86_64 one, for comparing the two); an
// override gets its own install.
bool install_app(Options* o, const fs::path& android_root) {
    std::error_code ec;
    const fs::path apps = android_root / "data" / "app";
    fs::create_directories(apps, ec);
    std::wstring abis = env_var(L"WINAPK_ABIS");
    const std::wstring tag = abis.empty() ? L"" : L"." + abis;
    if (abis.empty()) abis = L"x86_64,arm64-v8a";
    const fs::path record = apps / (L"." + o->apk.stem().wstring() + tag + L".installed");
    auto installed = [&]() -> fs::path {
        if (!fs::exists(record, ec) || fs::last_write_time(record, ec) < fs::last_write_time(o->apk, ec)) return {};
        FILE* f = _wfopen(record.c_str(), L"rb");
        if (!f) return {};
        std::string path;
        char buf[1024];
        for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) path.append(buf, n);
        std::fclose(f);
        const int len = MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(len > 0 ? len : 0), L'\0');
        if (len > 0) MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), &w[0], len);
        const fs::path base(w);
        return fs::exists(base, ec) ? base : fs::path();
    };
    fs::path base = installed();
    if (base.empty()) {
        std::fprintf(stderr, "[win.apk] installing the app (once per install)...\n");
        const int rc = run_java(*o, o->lib / "winapk-tools.jar", L"com.winapk.tools.AppInstall",
                                {o->apk.wstring(), android_root.wstring(), abis, record.wstring()});
        base = rc == 0 ? installed() : fs::path();
        if (base.empty()) {
            std::fprintf(stderr, "[win.apk] installing the app failed (exit code %d)\n", rc);
            return false;
        }
    }
    o->apk = base;
    return true;
}

// ---- arm64 translation, in the background ------------------------------------
//
// An app's arm64 libraries are translated ahead of time
// (include/winapk/arm64_aot.h): winapk-aot writes C, clang compiles it for
// this CPU into <lib>.so.aot.dll, which the loader picks up. Compiling a large
// library whole took most of an hour (libwaze.so, 1.5M instructions), and the
// app waited for it. Now nothing waits: the app starts on the JIT while a
// background process (this program, --aot-translate) does the work.
// A library of up to kAotFullBelow instructions (1 million: about a quarter
// of an hour of one core, once, then shared) is translated whole;
// a larger one only where its profile says code ran (arm64_aot.h,
// "Profiles"), and again when the profile has grown by a quarter, at least
// 200 lines, and no sooner than kAotMinIntervalS after the last time. Parts
// are of a fixed size, so a whole translation needs no more memory than one
// from a profile, only more time. A translation finished while
// the app runs is loaded at once (aot_load_pending); one that replaces a DLL
// in use waits as <lib>.so.aot.dll.new for the next start.
//
// The worker must not get in the user's way, which it did (issue #78: CPU
// and memory at 100% at every start of Waze; docs/research/
// background-translation.md and aot-measurements.md for how others do it and
// the measurements behind the numbers here):
//   - The C is cut in parts of kAotPartInsns instructions, however many cores
//     there are. clang's memory grows with a part (about 300 MB at 20,000
//     instructions, 1.7 GB at 176,000) and its speed does not; one part per
//     core had made a few huge ones.
//   - AotGovernor decides, every couple of seconds, how many clang run at
//     once: up to half the logical processors while the user is away, one
//     while the user is active, none on battery saver or under a full-screen
//     program, never so many that free memory falls below kAotReserve, and
//     no new one while the whole machine is above 80% busy.
//   - The worker and everything it starts run at idle priority, in
//     background mode (low I/O and memory priority) and as EcoQoS, in a job
//     object that ends them all when the worker ends and caps their memory
//     together (half the RAM, at least 2 GB) should the governor misjudge.
//   - The user can turn it off: Settings > Apps > "Optimize ARM apps in the
//     background" (HKCU\Software\Win.apk AotBackground, absent = 1). Then no
//     worker starts, and a running one stops before its next compile, keeping
//     its work for when it is on again. Translations already made are still
//     loaded (WINAPK_AOT=0 is the switch for that).
//   - Work survives interruption: a pass works in .aot-tmp\<lib>\ from a copy
//     of the profile taken when it began (`plan`, `profile`); each compiled
//     part is kept as p-<hash of its C>.o, so a worker stopped half way (the
//     PC shut down, the process killed) goes on where it was.
//   - A failed translation waits 10 minutes, then 4 times longer after each
//     failure (at most a day), and stops after kAotMaxFailures until the
//     library or winapk-aot changes.
//   - The running app starts the worker only once its profile has settled
//     (two flushes in a row without growth) or when it exits
//     (aot_app_exiting), and a worker started with the app waits
//     kAotStartDelayS before compiling, so the start stays fast.
//
// <lib>.so.aot.stamp (aot_stamp.h: its kinds, and who writes each) says what the last translation used: "full",
// "profile <lines>" (the profile's line count then), "wait <lines>" (too
// large without a profile, or failed: then with the number of failures and
// the Unix time of the next try), "skip" (not an arm64 ELF file: packed or
// encrypted, loaded by the app's own code), or "bad" (its translation
// crashed: the loader marks it, and the JIT runs the library; see
// arm64_aot.cpp). A DLL without a stamp is a whole translation from before
// stamps. The stamp holds the kAotVersion it was made with; another one means
// translate again. It ends with the library's id (aot_lib_id): a stamp for
// another build of the library is no stamp, and a DLL under it must be sealed.
//
// Many apps ship the same libraries (libc++_shared.so, libcrashlytics.so,
// SQLite, ...). As Windows on ARM's translation cache and Box64's DynaCache
// do, a whole translation is kept in <data>\aot-cache under a key of the
// library's code (the text hash the loader checks), winapk-aot and clang
// (each by size and date), the flags and the CPU, and an app with the same
// library links it (a second name for the same file, aot_link_or_copy)
// instead of compiling. Translations made from a profile differ by app and
// are not kept. The cache keeps to kAotCacheBytes, oldest use first, and
// drops what no app used for kAotCacheDays.
//
// Every app's worker writes that cache, its app reads the DLLs while they do,
// and any of them can be killed at any moment: the files are written as
// include/winapk/aot_files.h says (a DLL is sealed and checked, nothing is
// written in place, a good cache entry is never replaced, temporary names
// are unique and stale ones deleted). windows/test/aot_cache_test.cpp and
// test/aot_worker/run_aot_worker_wine.sh race and kill them.

constexpr size_t kAotFullBelow = 1000000;
constexpr int64_t kAotMinIntervalS = 3600;  // between two translations of a library from its profile
constexpr size_t kAotPartInsns = 20000;
constexpr uintmax_t kAotCacheBytes = uintmax_t{2} << 30;
constexpr int kAotCacheDays = 60;
constexpr uint64_t kAotJobBytes = uint64_t{512} << 20;  // memory to allow per clang (measured: 250-410 MB)
constexpr unsigned kAotMaxFailures = 4;
constexpr DWORD kAotStartDelayS = 60;
// -O2 with LLVM's fast instruction selector: about as fast to compile as -O1
// with Win.apk's clang 22 (97.2 s against 94.8 s over the same 11 parts of
// three libraries, on an i7-13620H; plain -O2 105 s), with 16% less code,
// and the code runs as fast (docs/research/aot-measurements.md).
const wchar_t* const kAotCompileFlags = L"-O2 -mllvm -fast-isel -march=native -fno-strict-aliasing -w";

fs::path aot_clang() {
    std::error_code ec;
    const fs::path set = env_var(L"WINAPK_AOT_CLANG");
    if (!set.empty() && fs::is_regular_file(set, ec)) return set;
    const fs::path bundled = exe_dir().parent_path() / "clang" / "bin" / "clang.exe";
    if (fs::is_regular_file(bundled, ec)) return bundled;
    wchar_t found[MAX_PATH * 4];
    if (SearchPathW(nullptr, L"clang.exe", nullptr, MAX_PATH * 4, found, nullptr) > 0) return found;
    return {};
}

// The bundled clang links with its own MinGW files (stage_clang.sh). Without a
// --sysroot, clang's MinGW driver takes them from the first gcc on the PATH
// (Strawberry Perl, MSYS2, a MinGW install), which has no libunwind, and every
// link fails. Another clang is left to find its own.
std::wstring aot_sysroot(const fs::path& clang) {
    std::error_code ec;
    const fs::path root = clang.parent_path().parent_path();
    if (!fs::equivalent(root, exe_dir().parent_path() / "clang", ec)) return {};
    return L" --sysroot=" + quoted(root);
}

fs::path arm64_libs(const fs::path& base) { return base.parent_path() / "lib" / "arm64"; }

fs::path with_suffix(const fs::path& so, const wchar_t* suffix) { return so.wstring() + suffix; }

// WINAPK_AOT_FULL_BELOW=<n> replaces kAotFullBelow (the worker's test makes
// a small library count as large with it).
size_t aot_full_below() {
    const std::wstring set = env_var(L"WINAPK_AOT_FULL_BELOW");
    return set.empty() ? kAotFullBelow : static_cast<size_t>(std::wcstoull(set.c_str(), nullptr, 10));
}

// HKCU\Software\Win.apk AotBackground (absent = 1): the launcher's Settings >
// Apps > "Optimize ARM apps in the background". Read again before each
// compile, so turning it off stops a running worker.
bool aot_background_on() {
    DWORD value = 1, size = sizeof value;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Win.apk", L"AotBackground", RRF_RT_REG_DWORD, nullptr, &value,
                     &size) != ERROR_SUCCESS) {
        return true;
    }
    return value != 0;
}

// Where a pass over `so` keeps its work until it is done.
fs::path aot_work_dir(const fs::path& so) { return so.parent_path() / ".aot-tmp" / so.filename(); }

int64_t unix_now() { return static_cast<int64_t>(std::time(nullptr)); }

std::string read_text(const fs::path& p) {
    std::string s;
    if (FILE* f = _wfopen(p.c_str(), L"rb")) {
        char buf[65536];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) s.append(buf, n);
        std::fclose(f);
    }
    return s;
}

bool write_text(const fs::path& p, const std::string& s) {
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (!f) return false;
    const bool ok = std::fwrite(s.data(), 1, s.size(), f) == s.size();
    return std::fclose(f) == 0 && ok;
}

uint64_t fnv1a(const std::string& s, uint64_t h = 1469598103934665603ull) {
    for (const unsigned char ch : s) h = (h ^ ch) * 1099511628211ull;
    return h;
}

std::string hex64(uint64_t v) {
    char out[24];
    std::snprintf(out, sizeof out, "%016llx", static_cast<unsigned long long>(v));
    return out;
}

// Lines of a profile file, without its header.
size_t profile_lines_of(const fs::path& profile) {
    FILE* f = _wfopen(profile.c_str(), L"rb");
    if (!f) return 0;
    size_t lines = 0;
    char buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) lines += std::count(buf, buf + n, '\n');
    std::fclose(f);
    return lines > 0 ? lines - 1 : 0;  // the header
}

size_t profile_lines(const fs::path& so) { return profile_lines_of(with_suffix(so, L".aot.profile")); }

// The stamp's kinds, its line and the rules that go by it are aot_stamp.h's.
using winapk::arm64::AotStamp;
using winapk::arm64::AotStampKind;

AotStamp read_aot_stamp(const fs::path& so) {
    return winapk::arm64::aot_parse_stamp(winapk::arm64::aot_read_file(with_suffix(so, L".aot.stamp")));
}

// Written whole or not at all (aot_files.h): the loader and the app's start
// read it while the worker writes it, and a worker killed half way through
// once left half a stamp.
void write_aot_stamp(const fs::path& so, AotStampKind kind, size_t lines, unsigned failures = 0,
                     int64_t retry_after = 0) {
    const std::string line =
        winapk::arm64::aot_stamp_line(kind, lines, winapk::arm64::kAotVersion, failures, retry_after,
                                      winapk::arm64::aot_cpu_signature(), winapk::arm64::aot_lib_id(so));
    if (!winapk::arm64::aot_write_file(with_suffix(so, L".aot.stamp"), line)) {
        std::fprintf(stderr, "[win.apk] %s: cannot write its stamp (error %lu)\n",
                     utf8(so.filename().wstring()).c_str(), GetLastError());
    }
}

bool newer_than(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::exists(a, ec) && fs::last_write_time(a, ec) >= fs::last_write_time(b, ec);
}

// Whether `so` needs a (new) translation now.
bool aot_needs_work(const fs::path& so) {
    std::error_code ec;
    const fs::path dll = with_suffix(so, L".aot.dll"), later = with_suffix(so, L".aot.dll.new");
    const AotStamp stamp = read_aot_stamp(so);
    bool have, stamp_current;
    if (stamp.lib != 0) {
        // The stamp names the library it is about, whatever the clock says,
        // and its DLLs are sealed: one cut short (a copy killed half way)
        // is no translation, and is made again.
        auto sealed = [](const fs::path& p) {
            return winapk::arm64::aot_check_dll(p, false) == winapk::arm64::AotFile::kSealed;
        };
        stamp_current = stamp.lib == winapk::arm64::aot_lib_id(so);
        have = stamp_current && (sealed(dll) || sealed(later));
    } else {  // a stamp from before: by the files' times
        have = newer_than(dll, so) || newer_than(later, so);
        stamp_current = newer_than(with_suffix(so, L".aot.stamp"), so);
    }
    // The rest is the stamp's own rules (aot_stamp_needs_work), given what the files say.
    winapk::arm64::AotStampFacts facts;
    facts.have = have;
    facts.current = stamp_current;
    facts.version = winapk::arm64::kAotVersion;
    facts.cpu = winapk::arm64::aot_cpu_signature();
    facts.now = unix_now();
    facts.pass_to_finish = fs::exists(aot_work_dir(so) / "plan", ec);
    facts.max_failures = kAotMaxFailures;
    facts.profile_lines = [&so] { return profile_lines(so); };
    return winapk::arm64::aot_stamp_needs_work(stamp, facts);
}

std::vector<fs::path> arm64_sos(const fs::path& libs) {
    std::vector<fs::path> sos;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(libs, ec)) {
        if (e.path().extension() == L".so") sos.push_back(e.path());
    }
    // Small ones first: they finish soonest.
    std::sort(sos.begin(), sos.end(), [](const fs::path& a, const fs::path& b) {
        std::error_code e1, e2;
        return fs::file_size(a, e1) < fs::file_size(b, e2);
    });
    return sos;
}

// A process the worker starts gets out of the user's way like the worker:
// EcoQoS (efficient cores, low clocks) and low memory priority. Idle
// priority and the job come from the worker by inheritance.
void background_child(HANDLE process) {
    PROCESS_POWER_THROTTLING_STATE eco{};
    eco.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    eco.ControlMask = eco.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    SetProcessInformation(process, ProcessPowerThrottling, &eco, sizeof eco);
    MEMORY_PRIORITY_INFORMATION memory{MEMORY_PRIORITY_LOW};
    SetProcessInformation(process, ProcessMemoryPriority, &memory, sizeof memory);
}

// How many compilers may run at once, decided again each time one is about
// to start (see the top of this section for the policy).
class AotGovernor {
public:
    size_t slots(size_t running) {
        const size_t logical = std::max(1u, std::thread::hardware_concurrency());
        // WINAPK_AOT_JOBS=<n>: n at most, whether or not the user is active
        // (to measure, or for a PC left to it); memory, battery saver and
        // full-screen programs still count.
        const size_t fixed = std::wcstoul(env_var(L"WINAPK_AOT_JOBS").c_str(), nullptr, 10);
        size_t n = fixed > 0 ? fixed : std::max<size_t>(1, logical / 2);
        const char* why = fixed > 0 ? "WINAPK_AOT_JOBS" : "the PC is idle";
        double busy = 0;
        FILETIME fi, fk, fu;
        if (GetSystemTimes(&fi, &fk, &fu)) {
            auto u64 = [](const FILETIME& t) {
                return static_cast<uint64_t>(t.dwHighDateTime) << 32 | t.dwLowDateTime;
            };
            const uint64_t idle = u64(fi), total = u64(fk) + u64(fu);  // kernel time includes idle time
            if (total_ != 0 && total > total_) busy = 1.0 - double(idle - idle_) / double(total - total_);
            idle_ = idle;
            total_ = total;
        }
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof ms;
        if (GlobalMemoryStatusEx(&ms)) {
            // Keep a fifth of the memory (at least 2 GB) for everything else.
            const uint64_t reserve = std::max<uint64_t>(uint64_t{2} << 30, ms.ullTotalPhys / 5);
            const uint64_t room = ms.ullAvailPhys > reserve ? ms.ullAvailPhys - reserve : 0;
            // Running compilers are already in ullAvailPhys: count them back.
            const size_t fit = static_cast<size_t>(room / kAotJobBytes) + running;
            if (fit < n) {
                n = fit;
                why = "free memory";
            }
        }
        LASTINPUTINFO li{};
        li.cbSize = sizeof li;
        if (fixed == 0 && n > 1 && GetLastInputInfo(&li) && GetTickCount() - li.dwTime < 2 * 60 * 1000) {
            n = 1;
            why = "the user is active";
        }
        if (running > 0 && busy > 0.80 && n > running) {
            n = running;
            why = "the PC is busy";
        }
        SYSTEM_POWER_STATUS power{};
        if (GetSystemPowerStatus(&power) && power.SystemStatusFlag == 1) {
            n = 0;
            why = "battery saver is on";
        }
        QUERY_USER_NOTIFICATION_STATE state;
        if (SUCCEEDED(SHQueryUserNotificationState(&state)) &&
            (state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE)) {
            n = 0;
            why = "a full-screen program runs";
        }
        if (n != last_) {
            std::fprintf(stderr, "[win.apk] aot: %zu compiler%s at once (%s)\n", n, n == 1 ? "" : "s", why);
            std::fflush(stderr);
            last_ = n;
        }
        return n;
    }

private:
    uint64_t idle_ = 0, total_ = 0;
    size_t last_ = SIZE_MAX;
};

struct Compile {
    std::wstring cmd;
    fs::path out;  // written as out + ".tmp", renamed to `out` when the compiler succeeds
};

enum class AotRun { kDone, kFailed, kStopped };

// Runs the compiles as the governor allows: kDone when every one succeeded,
// kStopped when the user turned the translation off before all had started
// (those running finish). A compile that fails leaves no object; the others
// are kept.
AotRun run_parallel(const std::vector<Compile>& compiles) {
    std::fflush(stderr);
    AotGovernor governor;
    struct Running {
        HANDLE process;
        const Compile* job;
    };
    std::vector<Running> running;
    bool ok = true, stopped = false;
    auto reap = [&](DWORD timeout) {
        std::vector<HANDLE> handles;
        for (const Running& r : running) handles.push_back(r.process);
        const DWORD w = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, timeout);
        if (w >= WAIT_OBJECT_0 + handles.size()) return;
        const Running r = running[w - WAIT_OBJECT_0];
        running.erase(running.begin() + (w - WAIT_OBJECT_0));
        DWORD code = 1;
        GetExitCodeProcess(r.process, &code);
        CloseHandle(r.process);
        const fs::path part = r.job->out.wstring() + L".tmp";
        if (code == 0 && MoveFileExW(part.c_str(), r.job->out.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
        std::error_code ec;
        fs::remove(part, ec);
        ok = false;
    };
    for (const Compile& c : compiles) {
        for (;;) {
            if (!aot_background_on()) {
                stopped = true;
                break;
            }
            const size_t allowed = std::min<size_t>(governor.slots(running.size()), MAXIMUM_WAIT_OBJECTS);
            if (running.size() < allowed) break;
            if (running.empty()) Sleep(2000);  // paused
            else reap(2000);
        }
        if (stopped) break;
        std::wstring cmd = c.cmd;
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, IDLE_PRIORITY_CLASS | CREATE_NO_WINDOW,
                            nullptr, nullptr, &si, &pi)) {
            std::fprintf(stderr, "[win.apk] aot: cannot start clang (error %lu)\n", GetLastError());
            ok = false;
            continue;
        }
        background_child(pi.hProcess);
        CloseHandle(pi.hThread);
        running.push_back({pi.hProcess, &c});
    }
    while (!running.empty()) reap(INFINITE);
    return stopped ? AotRun::kStopped : ok ? AotRun::kDone : AotRun::kFailed;
}

// Runs `cmdline` and returns its exit code (-1 if it did not start), with
// its standard output in *out.
int run_capture(std::wstring cmdline, std::string* out) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) return -1;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL started =
        CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(write);
    if (!started) {
        CloseHandle(read);
        return -1;
    }
    char buf[4096];
    for (DWORD n; ReadFile(read, buf, sizeof buf, &n, nullptr) && n > 0;) out->append(buf, n);
    CloseHandle(read);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

// The shared cache's name for a whole translation of `so`, or "" when `so`
// is not an arm64 ELF file winapk-aot can read.
std::string aot_cache_key(const fs::path& so, const fs::path& clang) {
    std::string hash;
    if (run_capture(quoted(exe_dir() / "winapk-aot.exe") + L" --hash " + quoted(so), &hash) != 0) return {};
    while (!hash.empty() && std::isspace(static_cast<unsigned char>(hash.back()))) hash.pop_back();
    // -march=native: the CPU too, for a data folder that moves to another PC.
    const std::string id = hash + "|" + file_id(exe_dir() / "winapk-aot.exe") + "|" + file_id(clang) + "|" +
                           utf8(kAotCompileFlags) + "|" + utf8(env_var(L"WINAPK_AOT_MAX_INSNS")) + "|" +
                           hex64(winapk::arm64::aot_cpu_signature());
    return hex64(fnv1a(id));
}

// Puts a finished translation in place, or, where the running app has the
// old one loaded and the system refuses to rename over it, beside it for the
// next start. A rename, so the app's name points at a whole DLL or at none.
bool install_aot_dll(const fs::path& dll, const fs::path& so) {
    const fs::path target = with_suffix(so, L".aot.dll"), later = with_suffix(so, L".aot.dll.new");
    winapk::arm64::aot_fault_point("install");
    const bool ok = winapk::arm64::aot_replace(dll, target) || winapk::arm64::aot_replace(dll, later);
    if (ok) winapk::arm64::aot_fault_point("installed");  // in place, the stamp not written yet
    return ok;
}

// A failure: "wait", tried again after a while that grows with each failure
// in a row (10 minutes, 40, 160, ..., at most a day).
void aot_failed(const fs::path& so, size_t lines) {
    const AotStamp before = read_aot_stamp(so);
    const unsigned failures = winapk::arm64::aot_stamp_failures_after(before);
    const int64_t wait = std::min<int64_t>(24 * 3600, int64_t{600} << (2 * std::min(failures - 1, 10u)));
    write_aot_stamp(so, AotStampKind::kWait, lines, failures, unix_now() + wait);
    if (failures >= kAotMaxFailures) {
        std::fprintf(stderr, "[win.apk] %s: %u failures in a row; not tried again until the library or Win.apk "
                             "changes (the JIT runs it)\n", utf8(so.filename().wstring()).c_str(), failures);
    } else {
        std::fprintf(stderr, "[win.apk] %s: tried again in %lld minutes\n", utf8(so.filename().wstring()).c_str(),
                     static_cast<long long>(wait / 60));
    }
}

// Translates one library: from the shared cache if it is there, else C from
// winapk-aot in parts, the parts compiled as the governor allows, linked into
// a DLL, put in place (and, when whole, into the cache). A pass interrupted
// half way goes on where it was the next time.
void aot_translate_one(const fs::path& so, const fs::path& clang, const fs::path& cache) {
    std::error_code ec;
    const fs::path work = aot_work_dir(so);
    const std::string name = utf8(so.filename().wstring());
    const ULONGLONG t0 = GetTickCount64();
    const std::string key = aot_cache_key(so, clang);
    if (key.empty()) {
        std::fprintf(stderr, "[win.apk] %s is not an arm64 ELF file (packed or encrypted; the app loads it "
                             "itself): the JIT runs its code\n", name.c_str());
        write_aot_stamp(so, AotStampKind::kSkip, 0);
        fs::remove_all(work, ec);
        return;
    }
    fs::create_directories(work, ec);
    const fs::path cached = cache.empty() ? fs::path() : cache / (key + ".aot.dll");
    const fs::path dll = work / (so.filename().wstring() + L".aot.dll");
    // aot_cache_take checks the entry whole and drops a broken one.
    if (!cached.empty() && winapk::arm64::aot_cache_take(cached, dll) && install_aot_dll(dll, so)) {
        write_aot_stamp(so, AotStampKind::kFull, profile_lines(so));
        fs::remove_all(work, ec);
        std::fprintf(stderr, "[win.apk] %s: translation taken from the shared cache\n", name.c_str());
        return;
    }
    // The pass: the one left half done for this build of the library and of
    // the tools, or a new one from a copy of the profile as it is now (the
    // profile may grow while the pass works; the next pass takes that).
    const fs::path plan = work / "plan", profile = work / "profile";
    const std::string plan_id = key + " " + file_id(so) + "\n";
    if (read_text(plan) == plan_id) {
        std::fprintf(stderr, "[win.apk] going on with the translation of %s, in the background...\n", name.c_str());
    } else {
        fs::remove_all(work, ec);
        fs::create_directories(work, ec);
        const fs::path live = with_suffix(so, L".aot.profile");
        if (fs::exists(live, ec) && !CopyFileW(live.c_str(), profile.c_str(), FALSE)) {
            std::fprintf(stderr, "[win.apk] %s: cannot copy the profile (error %lu)\n", name.c_str(), GetLastError());
        }
        if (!write_text(plan, plan_id)) {
            std::fprintf(stderr, "[win.apk] %s: cannot write %s (error %lu)\n", name.c_str(),
                         utf8(plan.wstring()).c_str(), GetLastError());
        }
        std::fprintf(stderr, "[win.apk] translating %s for this CPU, in the background...\n", name.c_str());
    }
    for (const auto& e : fs::directory_iterator(work, ec)) {  // what a stopped compile left
        if (e.path().extension() == L".tmp") fs::remove(e.path(), ec);
    }
    const size_t lines = profile_lines_of(profile);
    const fs::path c = work / (so.filename().wstring() + L".c");
    const int rc = run_process(quoted(exe_dir() / "winapk-aot.exe") + L" " + quoted(so) + L" " + quoted(c) +
                               L" --part-insns " + std::to_wstring(kAotPartInsns) + L" --profile " + quoted(profile) +
                               L" --full-below " + std::to_wstring(aot_full_below()));
    if (rc == 3) {
        std::fprintf(stderr, "[win.apk] %s: too large to translate whole; waiting for its profile\n", name.c_str());
        write_aot_stamp(so, AotStampKind::kWait, lines);
        fs::remove_all(work, ec);
        return;
    }
    bool ok = rc == 0 || rc == 4;
    std::vector<fs::path> objs;
    size_t reused = 0;
    if (ok) {
        // Each part's object is named by its C and how it is compiled, so a
        // part compiled before the pass was stopped is not compiled again.
        const std::string how =
            file_id(clang) + "|" + utf8(kAotCompileFlags) + "|" + utf8(env_var(L"WINAPK_AOT_MAX_INSNS"));
        std::vector<Compile> compiles;
        for (int i = 0;; ++i) {
            const fs::path src = i == 0 ? c : fs::path(c.wstring() + L"." + std::to_wstring(i) + L".c");
            if (!fs::exists(src, ec)) break;
            const fs::path obj = work / ("p-" + hex64(fnv1a(read_text(src), fnv1a(how))) + ".o");
            objs.push_back(obj);
            if (fs::exists(obj, ec)) {
                ++reused;
                continue;
            }
            compiles.push_back({quoted(clang) + aot_sysroot(clang) + L" " + kAotCompileFlags + L" -c -o " +
                                    quoted(fs::path(obj.wstring() + L".tmp")) + L" " + quoted(src),
                                obj});
        }
        if (reused > 0) {
            std::fprintf(stderr, "[win.apk] %s: %zu of %zu parts were compiled before\n", name.c_str(), reused,
                         objs.size());
        }
        const AotRun run = run_parallel(compiles);
        if (run == AotRun::kStopped) {  // the plan and the objects stay for when it is on again
            std::fprintf(stderr, "[win.apk] %s: stopped, background translation was turned off in Settings\n",
                         name.c_str());
            return;
        }
        ok = run == AotRun::kDone;
    }
    if (ok) {
        // A new file, never one written in place: `dll` may be a second name
        // of the cache's entry (taken, then not put in place), and a linker
        // that writes into its output would change that entry under every app.
        if (!winapk::arm64::aot_retire(dll)) {
            std::fprintf(stderr, "[win.apk] %s: cannot remove the old %s (error %lu)\n", name.c_str(),
                         utf8(dll.wstring()).c_str(), GetLastError());
            ok = false;
        }
    }
    if (ok) {
        std::wstring link = quoted(clang) + aot_sysroot(clang) + L" -shared -o " + quoted(dll);
        for (const fs::path& o : objs) link += L" " + quoted(o);
        ok = run_process(link) == 0;
        if (ok && !winapk::arm64::aot_seal(dll)) {
            std::fprintf(stderr, "[win.apk] %s: cannot seal the DLL (error %lu)\n", name.c_str(), GetLastError());
            ok = false;
        }
    }
    if (ok && rc == 0 && !cached.empty()) {  // whole: for every app with this library
        fs::create_directories(cache, ec);
        if (winapk::arm64::aot_cache_put(dll, cached)) {
            winapk::arm64::aot_cache_trim(cache, kAotCacheBytes, kAotCacheDays);
        }
    }
    if (ok && !install_aot_dll(dll, so)) {
        std::fprintf(stderr, "[win.apk] %s: cannot put the translation in place (error %lu)\n", name.c_str(),
                     GetLastError());
        ok = false;
    }
    if (!ok) {
        if (rc != 0 && rc != 4) {
            std::fprintf(stderr, "[win.apk] %s: translation failed (winapk-aot exit code %d); the JIT runs it\n",
                         name.c_str(), rc);
        } else {
            std::fprintf(stderr, "[win.apk] %s: translation failed (clang, above); the JIT runs it\n", name.c_str());
        }
        aot_failed(so, lines);
        // The objects that did compile stay for the next try of this pass,
        // unless there will be none.
        if (read_aot_stamp(so).failures >= kAotMaxFailures) fs::remove_all(work, ec);
        return;
    }
    fs::remove_all(work, ec);
    write_aot_stamp(so, rc == 4 ? AotStampKind::kProfile : AotStampKind::kFull, lines, 0,
                    rc == 4 ? unix_now() + kAotMinIntervalS : 0);
    std::fprintf(stderr, "[win.apk] %s: translated%s in %.1f s\n", name.c_str(), rc == 4 ? " (what ran)" : "",
                 static_cast<double>(GetTickCount64() - t0) / 1000);
}

std::wstring aot_mutex_name(const fs::path& libs) {
    std::wstring key = libs.lexically_normal().wstring();
    for (wchar_t& ch : key) ch = static_cast<wchar_t>(towlower(ch == L'/' ? L'\\' : ch));
    uint64_t h = 1469598103934665603ull;
    for (const wchar_t ch : key) h = (h ^ static_cast<uint16_t>(ch)) * 1099511628211ull;
    wchar_t name[48];
    swprintf(name, 48, L"Local\\WinApk-aot-%016llx", static_cast<unsigned long long>(h));
    return name;
}

// The worker and everything it starts: at idle priority, in background mode
// (low I/O and memory priority; for this process only), as EcoQoS, and in a
// job that ends them all when the worker ends, however it ends (before, a
// killed worker left its clang running and holding files in .aot-tmp).
void aot_worker_background() {
    SetPriorityClass(GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN);
    SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
    background_child(GetCurrentProcess());
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) ||
        !AssignProcessToJobObject(job, GetCurrentProcess())) {
        std::fprintf(stderr, "[win.apk] aot: no job object for the worker (error %lu): a compiler may outlive it\n",
                     GetLastError());
    } else {
        // A cap on the memory of the worker and its compilers together, in
        // case the governor misjudges (it counts free memory as it starts
        // each compiler): a compile over it fails and is tried again later,
        // the PC does not page. Set apart, so a system without job memory
        // limits still gets the kill on close.
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof ms;
        if (GlobalMemoryStatusEx(&ms)) {
            limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
            limits.JobMemoryLimit = static_cast<SIZE_T>(std::max<uint64_t>(uint64_t{2} << 30, ms.ullTotalPhys / 2));
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits)) {
                std::fprintf(stderr, "[win.apk] aot: no memory cap for the worker (error %lu)\n", GetLastError());
            }
        }
    }
    // The handle stays open until the process ends; closing it is what ends the job.
}

// winapk-runtime --aot-translate <lib\arm64 dir> [--cache <dir>] [--delay <s>]:
// the background worker. One per app; passes until nothing is left to do
// (the profile can grow while it works). --delay: the seconds to wait first,
// when the app has just started.
int aot_translate_main(const fs::path& libs, const fs::path& cache, DWORD delay_s) {
    HANDLE m = CreateMutexW(nullptr, FALSE, aot_mutex_name(libs).c_str());
    if (m && WaitForSingleObject(m, 0) == WAIT_TIMEOUT) return 0;  // another one is at it
    if (!aot_background_on()) {
        std::fprintf(stderr, "[win.apk] aot: background translation is off in Settings\n");
        return 0;
    }
    aot_worker_background();
    const fs::path clang = aot_clang();
    if (clang.empty()) {
        std::fprintf(stderr, "[win.apk] no clang found: arm64 code runs on the JIT\n");
        return 1;
    }
    if (delay_s > 0) Sleep(delay_s * 1000);
    if (!cache.empty()) winapk::arm64::aot_cache_trim(cache, kAotCacheBytes, kAotCacheDays);
    winapk::arm64::aot_remove_stale_temps(libs);  // stamps a killed worker or app was writing
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(libs / ".aot-tmp", ec)) {
        if (!e.is_directory(ec)) fs::remove(e.path(), ec);  // from before passes had a folder each
    }
    for (int pass = 0; pass < 8; ++pass) {
        bool worked = false;
        for (const fs::path& so : arm64_sos(libs)) {
            if (!aot_background_on()) return 0;  // turned off: what is done is kept
            if (!aot_needs_work(so)) continue;
            aot_translate_one(so, clang, cache);
            worked = true;
        }
        if (!worked) break;
    }
    return 0;
}

// Starts the worker for `libs` unless one is running; its output goes to
// `log` (appended).
void start_aot_worker(const fs::path& libs, const fs::path& log, const fs::path& cache, DWORD delay_s = 0) {
    if (!aot_background_on()) return;
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, aot_mutex_name(libs).c_str());
    if (m) {  // a worker holds it, or has just let go: either way it rescans
        CloseHandle(m);
        return;
    }
    std::error_code ec;
    fs::create_directories(log.parent_path(), ec);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return;
    // The log is the only handle it gets: the app's own (the launcher's pipe
    // for last-run.log) must not outlive the app in it.
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<uint8_t> attrs(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = out;
    if (InitializeProcThreadAttributeList(list, 1, 0, &size) &&
        UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &out, sizeof out, nullptr, nullptr)) {
        si.lpAttributeList = list;
    }
    std::wstring cmd = quoted(exe_dir() / "winapk-runtime.exe") + L" --aot-translate " + quoted(libs) + L" --cache " +
                       quoted(cache);
    if (delay_s > 0) cmd += L" --delay " + std::to_wstring(delay_s);
    const DWORD flags =
        IDLE_PRIORITY_CLASS | CREATE_NO_WINDOW | (si.lpAttributeList ? EXTENDED_STARTUPINFO_PRESENT : 0);
    PROCESS_INFORMATION pi{};
    // Out of the launcher's job, if it has one, so the work goes on after the app.
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, flags | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr,
                       &si.StartupInfo, &pi) ||
        CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si.StartupInfo, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (si.lpAttributeList) DeleteProcThreadAttributeList(list);
    CloseHandle(out);
}

bool any_aot_work(const fs::path& libs) {
    for (const fs::path& so : arm64_sos(libs)) {
        if (aot_needs_work(so)) return true;
    }
    return false;
}

struct AotWatch {
    fs::path libs, log, cache;
};

// The running app's, for aot_app_exiting; set once before the watch starts.
AotWatch* g_aot_watch = nullptr;

// While the app runs: writes the JIT's profile, starts the worker once it
// has settled, and loads translations as they are finished.
DWORD WINAPI aot_watch_thread(void* param) {
    const AotWatch w = *static_cast<AotWatch*>(param);
    delete static_cast<AotWatch*>(param);
    // Every 2 s for the first half minute (a flush with nothing new writes
    // nothing): an app that stops early (Waze, ten seconds in) still leaves
    // the profile of what ran. Then every 30 s. In the first sessions the
    // profile grows at nearly every flush, and a worker started at each one
    // translated it over and over: it starts once two flushes in a row
    // have added nothing (a minute without new code), a minute or more into
    // the run. A profile that never settles is taken at the next start.
    bool grew = false;
    int quiet = 0;
    for (int tick = 0;; ++tick) {
        Sleep(tick < 15 ? 2000 : 30000);
        if (winapk::arm64::aot_profile_flush() > 0) {
            grew = true;
            quiet = 0;
        } else if (tick >= 15 && ++quiet >= 2 && grew) {
            grew = false;
            if (any_aot_work(w.libs)) start_aot_worker(w.libs, w.log, w.cache);
        }
        winapk::arm64::aot_load_pending();
    }
}

// Step 1b, before the app's libraries load: puts in place what the worker
// finished while the app was not running, starts it for what is left (after
// kAotStartDelayS, when the app is starting), and watches the profile while
// the app runs. Never waits for a translation.
void prepare_arm64(const Options& o, const fs::path& app_dir) {
    std::error_code ec;
    const fs::path libs = arm64_libs(o.apk);
    if (!fs::is_directory(libs, ec) || !fs::exists(exe_dir() / "winapk-aot.exe", ec)) return;
    if (env_var(L"WINAPK_AOT") == L"0") {
        winapk::arm64::aot_disable();
        std::fprintf(stderr, "[win.apk] arm64 AOT: off (WINAPK_AOT=0); the JIT runs the app's arm64 code\n");
        return;
    }
    // Translation off for this start if the app crashed twice in a row with
    // translated code loaded (aot_guard.h). Then nothing is translated either:
    // the worker would only make what is not loaded.
    if (!o.install_only && !winapk::host::aot_guard_start(app_dir, winapk::host::aot_guard_id(o.apk, libs))) return;
    if (aot_clang().empty()) {
        std::fprintf(stderr, "[win.apk] no clang found (WINAPK_AOT_CLANG, clang\\bin\\clang.exe in Win.apk's folder, "
                             "or the PATH): the app's arm64 code runs on the JIT, which is slower\n");
        return;
    }
    for (const fs::path& so : arm64_sos(libs)) {
        const fs::path later = with_suffix(so, L".aot.dll.new");
        if (!fs::exists(later, ec)) continue;
        if (winapk::arm64::aot_check_dll(later, false) == winapk::arm64::AotFile::kBroken) {
            fs::remove(later, ec);  // aot_needs_work: translated again
            continue;
        }
        if (!winapk::arm64::aot_replace(later, with_suffix(so, L".aot.dll"), 2)) {
            std::fprintf(stderr, "[win.apk] cannot put %s in place (error %lu)\n", utf8(later.wstring()).c_str(),
                         GetLastError());
        }
    }
    const fs::path log = o.data / "logs" / (L"aot-" + app_dir.filename().wstring() + L".log");
    const fs::path cache = o.data / "aot-cache";
    if (any_aot_work(libs)) {
        if (!aot_background_on()) {
            std::fprintf(stderr, "[win.apk] arm64 code is left to the JIT: background translation is off in "
                                 "Settings\n");
        } else {
            std::fprintf(stderr, "[win.apk] arm64 code is being translated in the background (log: %s); "
                                 "until then the JIT runs it\n", utf8(log.wstring()).c_str());
            start_aot_worker(libs, log, cache, o.install_only ? 0 : kAotStartDelayS);
        }
    }
    if (!o.install_only) {
        g_aot_watch = new AotWatch{libs, log, cache};
        if (HANDLE t = CreateThread(nullptr, 0, aot_watch_thread, new AotWatch{libs, log, cache}, 0, nullptr)) CloseHandle(t);
    }
}

struct BootArgs {
    Options options;
    fs::path app_dir, app_jar;
    int result = 1;
};

// The display density, as upstream derives it from the Mac screen's point
// scale (Retina 2x = 320 dpi): the Windows scale of the monitor the app opens
// on x 160, so 100% is 160 (mdpi), 150% is 240 (hdpi), 200% is 320 (xhdpi).
// When the whole window would not fit that monitor's work area, the scale is
// lowered until it does (win32_window.h, window_plan): the app keeps its dp
// size and draws at exactly the window's pixel size, so nothing is stretched
// or shrunk. Headless runs (tests, CI) keep upstream's 320 so their frames
// stay 720x1280; WINAPK_DENSITY (a dpi) overrides both.
int display_density_dpi() {
    const std::wstring forced = env_var(L"WINAPK_DENSITY");
    if (!forced.empty()) return _wtoi(forced.c_str());
    if (!env_var(L"WINAPK_HEADLESS").empty()) return 320;
    int w = 0, h = 0;
    winapk::host::launch_phone_size(&w, &h);
    return winapk::host::window_density_dpi(winapk::host::window_plan(w, h));
}

std::vector<std::string> jvm_options(const BootArgs& b) {
    const Options& o = b.options;
    std::error_code ec;
    std::vector<std::string> v;
    v.push_back("-Djava.class.path=" + jvm_path(o.lib / "android-framework.jar") + ";" +
                jvm_path(o.lib / "macapk-loader.jar") + ";" + jvm_path(o.lib / "bcprov-jdk18on-1.79.jar") +
                (o.install_only || o.verify ? ";" + jvm_path(o.lib / "winapk-tools.jar") : std::string()));
    // Upstream's set (runtime/src/jvm_loader.cpp), minus the Mac-only parts.
    v.push_back("-Xrs");
    v.push_back("-Djava.library.path=");
    v.push_back("-XX:+UseSerialGC");
    v.push_back("-Xms128m");
    v.push_back("-Xmx1g");
    v.push_back("-Xss16m");
    v.push_back("-XX:+UnlockDiagnosticVMOptions");
    v.push_back("-XX:-BytecodeVerificationRemote");
    v.push_back("-XX:-BytecodeVerificationLocal");
    // C1 only, no C2: an app's start is mostly code that runs once, which
    // tiered compilation first profiles and then recompiles with C2 on the
    // other cores. Measured on windows-latest (4 CPUs), the first frame came
    // 25-35% sooner (Jerboa 2.08 -> 1.44 s, Fossify Notes 3.33 -> 2.45 s) for
    // less than half the CPU, while Mindustry and Shattered Pixel Dungeon held
    // 60 fps on the same CPU (+7%, -1%). Long hot Java loops lose C2's peak;
    // MACAPK_JVM_OPTS=-XX:TieredStopAtLevel=4, read after this, restores it.
    v.push_back("-XX:TieredStopAtLevel=1");
    // No hsperfdata file: HotSpot otherwise creates and maps a file per JVM in
    // %TEMP%\hsperfdata_<user> for jps/jstat, which nothing here reads. 6 ms
    // off creating the JVM (docs/research/startup-cds.md).
    v.push_back("-XX:-UsePerfData");
    v.push_back("--enable-native-access=ALL-UNNAMED");
    v.push_back("--add-opens=java.base/java.io=ALL-UNNAMED");
    v.push_back("--add-opens=java.base/java.util=ALL-UNNAMED");
    v.push_back("--add-opens=java.base/java.lang=ALL-UNNAMED");
    v.push_back("--add-opens=java.base/jdk.internal.loader=ALL-UNNAMED");
    v.push_back("--add-exports=java.base/sun.net.www.protocol.http=ALL-UNNAMED");
    v.push_back("--add-exports=java.base/sun.net.www.protocol.https=ALL-UNNAMED");
    if (fs::is_directory(o.lib / "java_base_patch", ec))
        v.push_back("--patch-module=java.base=" + jvm_path(o.lib / "java_base_patch"));
    if (fs::is_directory(o.lib / "jdk_unsupported_patch", ec))
        v.push_back("--patch-module=jdk.unsupported=" + jvm_path(o.lib / "jdk_unsupported_patch"));
    // Android's file system root (data/data/<app>, /proc mirror) and logs.
    const fs::path android_root = o.data / "android";
    const fs::path logs = o.data / "logs";
    fs::create_directories(android_root, ec);
    fs::create_directories(logs, ec);
    v.push_back("-Dmacapk.android.root=" + jvm_path(android_root));
    // What a process this runtime starts (the system broker, or an app the
    // broker starts as a service host) runs with: this same runtime, data
    // folder, JDK and jars (com.macapk.loader.WindowsBroker.runtimeCommand).
    {
        wchar_t self[MAX_PATH * 4];
        const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH * 4);
        if (n > 0 && n < MAX_PATH * 4) v.push_back("-Dwinapk.runtime.exe=" + jvm_path(fs::path(std::wstring(self, n))));
        v.push_back("-Dwinapk.data=" + jvm_path(o.data));
        v.push_back("-Dwinapk.jdk=" + jvm_path(o.jdk));
        v.push_back("-Dwinapk.lib=" + jvm_path(o.lib));
    }
    // Guest code's "/data/...", "/sdcard/..." resolve under the same folder.
    winapk::set_android_root(utf8(android_root.wstring()).c_str());
    // The app's data directory, by its 8.3 short name where the volume has
    // them. On a phone it is /data/user/0/<package>; here the full form is
    // ~150 characters (the profile folder, then the APK's file name), and
    // native code sized for the phone cuts it: Waze keeps "<dataDir>/waze" in
    // a 128-byte buffer and looked for its map scenes in a folder that does
    // not exist, so the map stayed an empty grid. Same folder, shorter name.
    if (!b.app_dir.empty()) {
        const fs::path data_dir = android_root / "data" / "data" / b.app_dir.filename();
        fs::create_directories(data_dir, ec);
        wchar_t buf[MAX_PATH * 4];
        const DWORD n = GetShortPathNameW(data_dir.c_str(), buf, MAX_PATH * 4);
        if (n > 0 && n < MAX_PATH * 4) v.push_back("-Dwinapk.data.dir=" + jvm_path(fs::path(std::wstring(buf, n))));
    }
    // Win.apk's own language, for the screens it draws itself (the Google
    // account chooser): its setting can differ from the Windows locale the app sees.
    v.push_back(std::string("-Dwinapk.ui.lang=") + (winapk::host::ui_hebrew() ? "he" : "en"));
    v.push_back("-Dmacapk.bundle.apk=" + jvm_path(o.apk));
    v.push_back("-Dmacapk.display.density=" + std::to_string(display_density_dpi()));
    v.push_back("-XX:ErrorFile=" + jvm_path(logs) + "\\hs_err_pid%p.log");
    // Extra options, space-separated, as upstream reads them.
    const std::string extra = utf8(env_var(L"MACAPK_JVM_OPTS"));
    for (size_t i = 0; i < extra.size();) {
        const size_t j = std::min(extra.find(' ', i), extra.size());
        if (j > i) v.push_back(extra.substr(i, j - i));
        i = j + 1;
    }
    // Class-data sharing (cds.h): last, so its key sees MACAPK_JVM_OPTS too.
    // Only an app's own start records the classes it loads.
    winapk::host::cds_apply(&v, o.jdk, o.lib, o.data,
                            o.install_only ? std::wstring() : b.app_dir.filename().wstring());
    return v;
}

void check_exception(JNIEnv* env, const char* what) {
    if (!env->ExceptionCheck()) return;
    std::fprintf(stderr, "[win.apk] exception in %s:\n", what);
    env->ExceptionDescribe();
    env->ExceptionClear();
}

// One line on how paths reach the JVM: the ANSI code page (65001 is UTF-8,
// which the manifest asks for) and the encoding the JVM takes file names in
// (sun.jnu.encoding).
void report_path_encoding(JNIEnv* env) {
    std::string jnu = "?";
    jclass sys = env->FindClass("java/lang/System");
    jmethodID get = sys ? env->GetStaticMethodID(sys, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;")
                        : nullptr;
    jstring v = get ? static_cast<jstring>(
                          env->CallStaticObjectMethod(sys, get, env->NewStringUTF("sun.jnu.encoding")))
                    : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (v) {
        const char* c = env->GetStringUTFChars(v, nullptr);
        if (c) {
            jnu = c;
            env->ReleaseStringUTFChars(v, c);
        }
    }
    std::fprintf(stderr, "[win.apk] ANSI code page %u; the JVM takes file names in %s\n", GetACP(), jnu.c_str());
}

// winapk-runtime --java-main --jdk <dir> --cp <jar> <class> [args...]
//
// `java -cp <jar> <class> args...` for run_java: creates a JVM with the
// default options (heap and all) and calls <class>.main with the arguments
// as UTF-16 Java strings. A System.exit in main ends the process with its
// code; otherwise the exit code is 0, or 1 when main throws, as with java.exe.
struct JavaMainArgs {
    fs::path jdk, cp;
    std::string main_class;  // binary name with '/', "com/macapk/dex/Main"
    std::vector<std::wstring> args;
    int result = 1;
};

DWORD WINAPI java_main_thread(void* param) {
    auto* a = static_cast<JavaMainArgs*>(param);
    HMODULE jvm = load_jvm(a->jdk);
    if (!jvm) return 1;
    std::vector<std::string> opts{"-Djava.class.path=" + jvm_path(a->cp)};
    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    if (!create_vm(jvm, opts, &vm, &env)) return 1;
    jclass cls = env->FindClass(a->main_class.c_str());
    jmethodID main = cls ? env->GetStaticMethodID(cls, "main", "([Ljava/lang/String;)V") : nullptr;
    if (!main) {
        check_exception(env, "the main class lookup");
        std::fprintf(stderr, "[win.apk] no main method in %s\n", a->main_class.c_str());
        return 1;
    }
    jobjectArray args =
        env->NewObjectArray(static_cast<jsize>(a->args.size()), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < a->args.size(); ++i) {
        env->SetObjectArrayElement(args, static_cast<jsize>(i), jstring_of(env, a->args[i]));
    }
    env->CallStaticVoidMethod(cls, main, args);
    const bool threw = env->ExceptionCheck();
    if (threw) {
        // As java.exe reports it: the thread's uncaught exception handler
        // prints "Exception in thread "main" ...".
        jthrowable t = env->ExceptionOccurred();
        env->ExceptionClear();
        jclass thread_cls = env->FindClass("java/lang/Thread");
        jmethodID current = env->GetStaticMethodID(thread_cls, "currentThread", "()Ljava/lang/Thread;");
        jmethodID dispatch = env->GetMethodID(thread_cls, "dispatchUncaughtException", "(Ljava/lang/Throwable;)V");
        jobject self = current ? env->CallStaticObjectMethod(thread_cls, current) : nullptr;
        if (self && dispatch) env->CallVoidMethod(self, dispatch, t);
        if (env->ExceptionCheck()) env->ExceptionDescribe();
        env->ExceptionClear();
    }
    vm->DestroyJavaVM();  // waits for the program's other threads, as java.exe does
    a->result = threw ? 1 : 0;
    return 0;
}

int java_main(int argc, wchar_t** argv) {
    JavaMainArgs a;
    int i = 2;
    for (; i + 1 < argc; i += 2) {
        const std::wstring k = argv[i];
        if (k == L"--jdk") a.jdk = argv[i + 1];
        else if (k == L"--cp") a.cp = argv[i + 1];
        else break;
    }
    if (a.jdk.empty() || a.cp.empty() || i >= argc) {
        std::fprintf(stderr, "usage: winapk-runtime --java-main --jdk <dir> --cp <jar> <class> [args...]\n");
        return 2;
    }
    a.main_class = utf8(argv[i++]);
    std::replace(a.main_class.begin(), a.main_class.end(), '.', '/');
    for (; i < argc; ++i) a.args.emplace_back(argv[i]);
    // HotSpot's main thread is the one that creates the VM: give it at least
    // java.exe's room.
    HANDLE t = CreateThread(nullptr, 64 * 1024 * 1024, java_main_thread, &a, STACK_SIZE_PARAM_IS_A_RESERVATION,
                            nullptr);
    if (!t) return 1;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    std::fflush(stderr);
    return a.result;
}

// winapk-runtime --gms <subcommand> [args]: upstream's `macapk-runtime --gms`
// (runtime/src/main.cpp, RunGmsCommand). A JVM with the framework and loader
// jars and no app, the host natives bound (`--gms signin-window` shows the
// Google sign-in window through NativeSignIn; `accounts` needs none), and the
// arguments handed to com.macapk.gms.GmsCli.run, the one entry point every
// GMS operation has. Its exit code is a contract the launcher's Settings
// reads: 0 done, 1 an `ERROR\t<message>` line was printed, 2 cancelled or
// wrong arguments. The process exits without a VM shutdown, as upstream: a
// sign-in run has WebView2's processes behind it and nothing needs a shutdown
// hook (GmsStore writes its file on every mutation).
//
// winapk-runtime --verify-apk <app> runs here too, with winapk-tools.jar on
// the class path: com.winapk.tools.ApkCheck prints the file's package and
// whether its signature verifies and Google signed it, its native code's ABIs
// and minSdkVersion, and the newest API level Win.apk presents (the launcher's
// Google services setup installs only a Google-signed file that has x86_64
// code, if any, and does not need a newer level). Exit codes: 0 Google-signed,
// 4 another signer, 3 does not verify, 1 not an app, 2 usage.
DWORD WINAPI gms_thread(void* param) {
    auto* b = static_cast<BootArgs*>(param);
    const Options& o = b->options;
    b->result = 1;
    HMODULE jvm = load_jvm(o.jdk);
    if (!jvm) return 1;
    std::vector<std::string> opts = jvm_options(*b);
    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    if (!create_vm(jvm, opts, &vm, &env)) return 1;
    jvmtiEnv* jvmti = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_2) != JNI_OK) {
        std::fprintf(stderr, "[win.apk] no JVMTI\n");
        return 1;
    }
    winapk::host::NativeStats stats;
    if (!winapk::host::register_host_natives(env, jvmti, &stats)) {
        std::fprintf(stderr, "[win.apk] binding host natives failed\n");
        return 1;
    }
    macapk::sqlite::RegisterNativeSqliteMethods(env);
    check_exception(env, "RegisterNativeSqliteMethods");

    // --verify-apk is the same shape: com.winapk.tools.ApkCheck.run(String[])int, on the one file.
    const char* cls_name = o.verify ? "com/winapk/tools/ApkCheck" : "com/macapk/gms/GmsCli";
    const std::vector<std::wstring> cli_args = o.verify ? std::vector<std::wstring>{o.apk.wstring()} : o.gms_args;
    jclass cli = env->FindClass(cls_name);
    jmethodID run = cli ? env->GetStaticMethodID(cli, "run", "([Ljava/lang/String;)I") : nullptr;
    if (!run) {
        check_exception(env, "run lookup");
        std::fprintf(stderr, "ERROR\t%s.run(String[])int is missing from the jars\n", cls_name);
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 1);
    }
    jobjectArray args = env->NewObjectArray(static_cast<jsize>(cli_args.size()), env->FindClass("java/lang/String"),
                                            nullptr);
    for (size_t i = 0; i < cli_args.size(); ++i) {
        env->SetObjectArrayElement(args, static_cast<jsize>(i), jstring_of(env, cli_args[i]));
    }
    jint rc = env->CallStaticIntMethod(cli, run, args);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        rc = 1;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(rc));
    return 0;
}

// winapk-runtime --system: com.macapk.loader.SystemServer.main in a JVM with
// the host natives (the Binder transport among them) and no app. main serves
// until the broker decides it is idle, and then exits the process itself.
DWORD WINAPI system_thread(void* param) {
    auto* b = static_cast<BootArgs*>(param);
    const Options& o = b->options;
    b->result = 1;
    HMODULE jvm = load_jvm(o.jdk);
    if (!jvm) return 1;
    std::vector<std::string> opts = jvm_options(*b);
    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    if (!create_vm(jvm, opts, &vm, &env)) return 1;
    jvmtiEnv* jvmti = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_2) != JNI_OK) {
        std::fprintf(stderr, "[win.apk] no JVMTI\n");
        return 1;
    }
    winapk::host::NativeStats stats;
    if (!winapk::host::register_host_natives(env, jvmti, &stats)) {
        std::fprintf(stderr, "[win.apk] binding host natives failed\n");
        return 1;
    }
    macapk::sqlite::RegisterNativeSqliteMethods(env);
    check_exception(env, "RegisterNativeSqliteMethods");
    jclass server = env->FindClass("com/macapk/loader/SystemServer");
    jmethodID main = server ? env->GetStaticMethodID(server, "main", "([Ljava/lang/String;)V") : nullptr;
    if (!main) {
        check_exception(env, "SystemServer.main lookup");
        return 1;
    }
    jobjectArray args = env->NewObjectArray(0, env->FindClass("java/lang/String"), nullptr);
    env->CallStaticVoidMethod(server, main, args);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 1);
    return 0;
}

// Steps 3 to 5, on the large-stack thread.
DWORD WINAPI boot_thread(void* param) {
    auto* b = static_cast<BootArgs*>(param);
    const Options& o = b->options;

    HMODULE jvm = load_jvm(o.jdk);
    if (!jvm) return 1;
    std::vector<std::string> opts = jvm_options(*b);
    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    if (!create_vm(jvm, opts, &vm, &env)) return 1;
    report_path_encoding(env);
    jvmtiEnv* jvmti = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_2) != JNI_OK) {
        std::fprintf(stderr, "[win.apk] no JVMTI\n");
        return 1;
    }

    winapk::host::NativeStats stats;
    if (!winapk::host::register_host_natives(env, jvmti, &stats)) {
        std::fprintf(stderr, "[win.apk] binding host natives failed\n");
        return 1;
    }
    std::fprintf(stderr, "[win.apk] host natives: %d classes, %d implemented, %d stubbed\n", stats.classes,
                 stats.implemented, stats.stubbed);
    // The app's own native libraries load through the guest linker.
    if (!winapk::host::guest_libs_init(env, jvmti)) {
        std::fprintf(stderr, "[win.apk] native library support is unavailable for this run\n");
    }
    // Upstream's own NativeSqlite bridge replaces those stubs (see CMakeLists).
    macapk::sqlite::RegisterNativeSqliteMethods(env);
    check_exception(env, "RegisterNativeSqliteMethods");

    auto jstr = [&](const fs::path& p) { return jstring_of(env, p.wstring()); };

    if (o.install_only) {
        jclass meta = env->FindClass("com/winapk/tools/AppMeta");
        jmethodID write = meta ? env->GetStaticMethodID(meta, "write", "(Ljava/lang/String;Ljava/lang/String;)V")
                               : nullptr;
        if (write) env->CallStaticVoidMethod(meta, write, jstr(o.apk), jstr(b->app_dir));
        check_exception(env, "AppMeta.write");
        // Framework threads the icon render started must not keep the
        // process alive.
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), write ? 0 : 1);
    }

    jclass boot = env->FindClass("com/macapk/loader/Boot");
    check_exception(env, "FindClass com.macapk.loader.Boot");
    jmethodID run = boot ? env->GetStaticMethodID(
                               boot, "run",
                               "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V")
                         : nullptr;
    if (!run) {
        check_exception(env, "Boot.run lookup");
        return 1;
    }
    // The manifest argument names a decoded text manifest, which Windows does
    // not produce; Manifest.parse then reads the binary one from the APK.
    env->CallStaticVoidMethod(boot, run, jstr(o.apk), jstr(b->app_jar), jstr(b->app_dir),
                              jstr(b->app_dir / "AndroidManifest.txt"));
    check_exception(env, "Boot.run");

    // Returns once the app's non-daemon threads are done.
    vm->DestroyJavaVM();
    b->result = 0;
    return 0;
}

// One runtime per app at a time. Two runtimes on the same app share its
// databases, preferences and, for a messenger, its identity keys: SQLite's
// locking keeps the files intact only as far as each process expects to be
// alone, and an app's own state (a message counter, a ratchet) breaks at once.
// Upstream's app stub holds run/<app>.pid for the runtime's life and a second
// launch only activates the running app; a shortcut, the library and a script
// here could each start a second copy.
//
// The lock is a named mutex keyed on the app's data folder (absolute,
// lower-cased), held until the process exits; a mutex left by a process that
// died counts as free (WAIT_ABANDONED). Next to it, a named mapping holds the
// holder's process id, so a second start can find the holder's window.
// WINAPK_ALLOW_SECOND_INSTANCE=1 turns the check off.
std::wstring instance_key(const fs::path& app_dir) {
    std::error_code ec;
    std::wstring key = fs::absolute(app_dir, ec).lexically_normal().wstring();
    if (ec) key = app_dir.wstring();
    // A package's other android:process (the system broker starts it with
    // MACAPK_SECONDARY_PROCESS=1 and its name) is a second process of the same
    // app, not a second copy of it.
    if (!env_var(L"MACAPK_SECONDARY_PROCESS").empty() && !env_var(L"MACAPK_PROCESS_NAME").empty())
        key += L"|" + env_var(L"MACAPK_PROCESS_NAME");
    while (!key.empty() && (key.back() == L'\\' || key.back() == L'/')) key.pop_back();
    for (wchar_t& c : key) c = static_cast<wchar_t>(towlower(c == L'/' ? L'\\' : c));
    uint64_t h = 1469598103934665603ull;  // FNV-1a over the UTF-16 units
    for (const wchar_t c : key) h = (h ^ static_cast<uint16_t>(c)) * 1099511628211ull;
    wchar_t name[48];
    swprintf(name, 48, L"Local\\WinApk-app-%016llx", static_cast<unsigned long long>(h));
    return name;
}

// The holder's flags, after its pid in the "-pid" mapping.
constexpr DWORD kHolderServiceHost = 1;

bool claim_single_instance(const std::wstring& key) {
    if (!env_var(L"WINAPK_ALLOW_SECOND_INSTANCE").empty()) return true;
    HANDLE m = CreateMutexW(nullptr, FALSE, key.c_str());
    if (!m) return true;  // no lock to be had: do not block the start
    const DWORD w = WaitForSingleObject(m, 0);
    if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) {
        CloseHandle(m);
        return false;
    }
    // Held until exit, like the mapping that names us: our pid, then whether
    // we are a service host (kHolderServiceHost).
    if (HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 2 * sizeof(DWORD),
                                        (key + L"-pid").c_str())) {
        if (auto* p = static_cast<DWORD*>(MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 2 * sizeof(DWORD)))) {
            p[0] = GetCurrentProcessId();
            p[1] = env_var(L"MACAPK_SERVICE_HOST") == L"1" ? kHolderServiceHost : 0;
        }
    }
    return true;
}

// A user's start of an app whose process the system broker started as a
// service host (to answer another app's bind or broadcast: Play services binds
// the Play Store's, for one). On a device the activity would start in that
// process; ours runs headless, with no window to show it in, so the start
// would be refused and nothing would appear. Stop the host and let this start
// take its place: its clients see the binder die and bind again, which the
// broker answers from the new process, as when Android kills a background
// process. True when the holder was a service host and has exited.
bool replace_service_host(const std::wstring& key) {
    if (env_var(L"MACAPK_SERVICE_HOST") == L"1") return false;  // the broker's own start: the holder serves it
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, (key + L"-pid").c_str());
    if (!map) return false;
    DWORD pid = 0, flags = 0;
    if (const auto* p = static_cast<const DWORD*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 2 * sizeof(DWORD)))) {
        pid = p[0];
        flags = p[1];
        UnmapViewOfFile(p);
    }
    CloseHandle(map);
    if (!pid || !(flags & kHolderServiceHost)) return false;
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    std::fprintf(stderr, "[win.apk] the system broker runs this app as a service host (pid %lu); "
                         "restarting it with a window\n", static_cast<unsigned long>(pid));
    TerminateProcess(h, 1);
    const bool gone = WaitForSingleObject(h, 15000) == WAIT_OBJECT_0;
    CloseHandle(h);
    return gone;
}

struct WindowSearch {
    DWORD pid;
    HWND found;
};

BOOL CALLBACK find_app_window(HWND w, LPARAM lp) {
    auto* f = reinterpret_cast<WindowSearch*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    wchar_t cls[32] = {};
    GetClassNameW(w, cls, 32);
    if (pid != f->pid || !IsWindowVisible(w) || wcscmp(cls, L"WinApkAppWindow") != 0) return TRUE;
    f->found = w;
    return FALSE;
}

// Brings the running copy's window forward. It may still be starting, with
// no window yet, so this waits a little for one.
void activate_running(const std::wstring& key) {
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, (key + L"-pid").c_str());
    if (!map) return;
    DWORD pid = 0;
    if (const auto* p = static_cast<const DWORD*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(DWORD)))) {
        pid = *p;
        UnmapViewOfFile(p);
    }
    CloseHandle(map);
    if (!pid) return;
    AllowSetForegroundWindow(pid);
    for (int i = 0; i < 60; ++i) {
        WindowSearch f{pid, nullptr};
        EnumWindows(find_app_window, reinterpret_cast<LPARAM>(&f));
        if (f.found) {
            if (IsIconic(f.found)) ShowWindow(f.found, SW_RESTORE);
            SetForegroundWindow(f.found);
            return;
        }
        Sleep(250);
    }
}

}  // namespace

namespace winapk::host {
// The app is exiting (Cocoa_exitProcess): its profile has settled. Writes
// what the JIT ran since the last flush and starts the worker at once, with
// no start delay, for what that leaves to do.
void aot_app_exiting() {
    if (!g_aot_watch) return;
    winapk::arm64::aot_profile_flush();
    if (any_aot_work(g_aot_watch->libs)) start_aot_worker(g_aot_watch->libs, g_aot_watch->log, g_aot_watch->cache);
}
}  // namespace winapk::host

// The volume's Download, Pictures, Documents, Music and Movies are the user's
// Windows folders (user_folders.h). Only for the user's own library: a run
// with --data (tests, the compat sweep, CI) must never touch real folders.
// MACAPK_USER_FOLDERS decides when set (=0 changes nothing, as upstream);
// otherwise the launcher's Settings switch (UserFolders, default on), whose
// off undoes the junctions so it visibly does what it says.
void map_shared_folders(const Options& o) {
    if (!o.default_data) return;
    using winapk::host::UserFolderMode;
    UserFolderMode mode = UserFolderMode::kMap;
    const std::wstring forced = env_var(L"MACAPK_USER_FOLDERS");
    if (!forced.empty()) {
        if (forced[0] == L'0') mode = UserFolderMode::kKeep;
    } else {
        DWORD on = 1, size = sizeof on;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Win.apk", L"UserFolders", RRF_RT_REG_DWORD, nullptr, &on,
                         &size) == ERROR_SUCCESS &&
            on == 0) {
            mode = UserFolderMode::kUnmap;
        }
    }
    const fs::path android_root = o.data / "android";
    const auto mapped =
        winapk::host::map_user_folders(android_root / "sdcard", winapk::host::user_folders(),
                                       android_root / "data" / "system" / "storage" / "usermap-journal.tsv", mode);
    // Set even when empty: its presence tells the Java side the host owns the
    // mapping, so SharedStorage.mapUserFolders never runs here.
    SetEnvironmentVariableW(L"WINAPK_USER_FOLDER_TARGETS", winapk::host::user_folder_env(mapped).c_str());
}

// stderr is unbuffered in the UCRT, and an unbuffered fprintf hands the
// stream ONE CHARACTER per WriteFile: measured 564 us per 150-byte log line
// against 9 us for the same line through a buffer, all of it under the stream
// lock. Every log line of every thread queued behind that lock, and lines from
// different threads interleaved character by character. A JFR of WhatsApp's
// boot had its main thread in NativeLog.print for most of the samples it was
// not idle in: the first frame took 48 to 76 s, and about 5 s with this.
//
// So stderr gets a 64 KB buffer, flushed every 50 ms by a background thread,
// at once for warnings and errors (NativeLog), and on the exit paths that
// already flush. A hard crash can lose at most the last 50 ms of INFO lines;
// the interval is that short because an abort in a library with its own C
// runtime (ANGLE's) ends the process without flushing ours, and the log's
// last lines are the ones that say why (VLC's run ended that way with
// nothing after a class load).
DWORD WINAPI stderr_flusher(void*) {
    for (;;) {
        Sleep(50);
        std::fflush(stderr);
    }
}

void buffer_stderr() {
    // A headless run (the sweep, the tests) wants every line the moment it is
    // written: an exit through a library's own C runtime, or a kill, drops
    // what is buffered, and those last lines are what the log is read for
    // (VLC's run ended with nothing after a class load). Speed matters there
    // less than the line that says why.
    if (!env_var(L"WINAPK_HEADLESS").empty()) {
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        return;
    }
    static char buf[1 << 16];
    std::setvbuf(stderr, buf, _IOFBF, sizeof buf);
    HANDLE t = CreateThread(nullptr, 64 * 1024, stderr_flusher, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (t) CloseHandle(t);
}

int wmain(int argc, wchar_t** argv) {
    buffer_stderr();
    winapk::host::install_crash_report();
    winapk::host::take_launcher_ready_event();
    // A relaunch (Cocoa_exitProcess) starts before the old process has ended:
    // wait for it, so the single-instance claim below finds the app gone.
    if (const std::wstring after = env_var(L"WINAPK_RELAUNCH_AFTER"); !after.empty()) {
        SetEnvironmentVariableW(L"WINAPK_RELAUNCH_AFTER", nullptr);
        const DWORD pid = static_cast<DWORD>(std::wcstoul(after.c_str(), nullptr, 10));
        if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
            WaitForSingleObject(old, 15000);
            CloseHandle(old);
        }
    }
    if (argc > 1 && std::wstring(argv[1]) == L"--java-main") return java_main(argc, argv);
    if (argc > 1 && std::wstring(argv[1]) == L"--cds-dump") return winapk::host::cds_dump_main(argc, argv);
    if (argc >= 3 && std::wstring(argv[1]) == L"--aot-translate") {
        fs::path cache;
        DWORD delay_s = 0;
        for (int i = 3; i + 1 < argc; i += 2) {
            if (std::wstring(argv[i]) == L"--cache") cache = argv[i + 1];
            else if (std::wstring(argv[i]) == L"--delay")
                delay_s = static_cast<DWORD>(std::wcstoul(argv[i + 1], nullptr, 10));
        }
        return aot_translate_main(argv[2], cache, delay_s);
    }
    BootArgs b;
    if (!parse_args(argc, argv, &b.options)) {
        std::fprintf(stderr, "usage: winapk-runtime --apk <file.apk|.apkm|.xapk|.apks> [--data <dir>] [--jdk <dir>] [--lib <dir>] "
                     "[--install-only] [--package-to <file>]\n"
                     "       winapk-runtime [--data <dir>] [--jdk <dir>] [--lib <dir>] --gms <subcommand> [args]\n"
                     "       winapk-runtime [--data <dir>] [--jdk <dir>] [--lib <dir>] --system\n"
                     "       winapk-runtime [--data <dir>] [--jdk <dir>] [--lib <dir>] --verify-apk <app>\n"
                     "       winapk-runtime [--data <dir>] [--jdk <dir>] [--lib <dir>] --cds-prepare\n");
        return 2;
    }
    if (!resolve_paths(&b.options)) return 2;
    if (b.options.cds_prepare) {
        jvm_options(b);  // the options, and with them the archive's key, an app's start would have
        return winapk::host::cds_prepare();
    }
    // A crash from here on also leaves logs\crash-<app>-<time>.dmp.
    winapk::host::set_crash_dump_location(b.options.data / "logs",
                                          b.options.apk.empty() ? L"runtime" : b.options.apk.stem().wstring());
    if (!b.options.package_to.empty()) {
        return run_java(b.options, b.options.lib / "winapk-tools.jar", L"com.winapk.tools.AppInstall",
                        {L"--package", b.options.apk.wstring(), b.options.package_to.wstring()});
    }

    // F-Droid and other stores install through PackageInstaller, which hands
    // the APK to the install helper (PackageInstallerBackend), as on the Mac:
    // here the helper is the launcher, Win.apk.exe install --apk ... It keeps
    // the installed-apps registry PackageManager answers from, and stages
    // under the data folder.
    {
        std::error_code ec0;
        const fs::path launcher = exe_dir().parent_path() / L"Win.apk.exe";
        if (env_var(L"MACAPK_INSTALLHELPER").empty() && fs::is_regular_file(launcher, ec0))
            SetEnvironmentVariableW(L"MACAPK_INSTALLHELPER", launcher.c_str());
        if (env_var(L"MACAPK_PM_REGISTRY_FILE").empty())
            SetEnvironmentVariableW(L"MACAPK_PM_REGISTRY_FILE",
                                    (b.options.data / L"installed_apps.json").c_str());
        if (env_var(L"MACAPK_STAGING_DIR").empty())
            SetEnvironmentVariableW(L"MACAPK_STAGING_DIR", (b.options.data / L"staging").c_str());
        // The GMS layer's state (the signed-in Google account, its tokens, the
        // checkin identity: com.macapk.gms.GmsStore) next to the registry, as
        // upstream keeps it next to installed_apps.json; its default is a Mac
        // path under the home directory.
        if (env_var(L"MACAPK_GMS_DIR").empty())
            SetEnvironmentVariableW(L"MACAPK_GMS_DIR", (b.options.data / L"gms").c_str());
        // No line per class loaded (AppClassLoader.QUIET_CLASSLOAD), as
        // upstream's launcher sets it: 1,000-6,000 lines a start, 0.05-0.4 s
        // before the first frame. MACAPK_QUIET_CLASSLOAD=0 brings them back.
        if (env_var(L"MACAPK_QUIET_CLASSLOAD").empty()) SetEnvironmentVariableW(L"MACAPK_QUIET_CLASSLOAD", L"1");
        // Settings > Google account > "Use Google services (experimental)":
        // the system broker and the real Google apps (SystemClient.enabled).
        // MACAPK_BROKER, when set, wins either way.
        if (env_var(L"MACAPK_BROKER").empty() && google_services_setting())
            SetEnvironmentVariableW(L"MACAPK_BROKER", L"1");
    }

    // --system: the broker. One per user session: a second one exits at once.
    if (b.options.system) {
        HANDLE one = CreateMutexW(nullptr, TRUE, L"Local\\WinApk-system");
        if (!one || GetLastError() == ERROR_ALREADY_EXISTS) {
            std::fprintf(stderr, "[win.apk] the system broker is already running\n");
            return 0;
        }
        SetEnvironmentVariableW(L"MACAPK_BROKER", L"1");
        SetEnvironmentVariableW(L"MACAPK_APK_PATH", nullptr);  // the app that started it, not the broker's
        if (env_var(L"MACAPK_BROKER_LOGDIR").empty()) {
            std::error_code lec;
            fs::create_directories(b.options.data / L"logs" / L"services", lec);
            SetEnvironmentVariableW(L"MACAPK_BROKER_LOGDIR", (b.options.data / L"logs" / L"services").c_str());
        }
        HANDLE t = CreateThread(nullptr, 64 * 1024 * 1024, system_thread, &b, STACK_SIZE_PARAM_IS_A_RESERVATION,
                                nullptr);
        if (!t) return 1;
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
        return b.result;
    }

    // --gms: a JVM, the GMS classes and no app. Before anything APK-shaped.
    // --verify-apk too: it only reads the file.
    if (b.options.gms || b.options.verify) {
        SetEnvironmentVariableW(L"MACAPK_APK_PATH", nullptr);
        HANDLE t = CreateThread(nullptr, 64 * 1024 * 1024, gms_thread, &b, STACK_SIZE_PARAM_IS_A_RESERVATION,
                                nullptr);
        if (!t) return 1;
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
        return b.result;
    }

    // Permissions ask as on a device: the storage and location runtime
    // permissions, "All files access" and "Media management" are answered by
    // the launcher's permission dialogs (Win.apk.exe runtime-permission,
    // special-access, app-details), which sign the answer into
    // <data>\appops\app-ops.json for SpecialAccessStore, as
    // macapk-installhelper does on the Mac. MACAPK_ALLFILES_HONEST=0 still
    // restores the old answer: every app holds all files access.

    if (!b.options.install_only) map_shared_folders(b.options);

    std::error_code ec;
    b.app_dir = b.options.data / "apps" / b.options.apk.stem();
    winapk::host::set_app_dir(b.app_dir.wstring());
    // Before the window: the taskbar and the notifications go by it.
    if (!b.options.install_only) winapk::host::set_app_user_model_id(b.options.apk.stem().wstring());
    fs::create_directories(b.app_dir, ec);
    if (ec) {
        std::fprintf(stderr, "[win.apk] cannot create %s\n", utf8(b.app_dir.wstring()).c_str());
        return 1;
    }
    const std::wstring instance = instance_key(b.app_dir);
    if (!claim_single_instance(instance) &&
        (b.options.install_only || !replace_service_host(instance) || !claim_single_instance(instance))) {
        std::fprintf(stderr, "[win.apk] %s is already running; not starting a second copy\n",
                     utf8(b.app_dir.filename().wstring()).c_str());
        if (b.options.install_only) return 3;
        if (env_var(L"WINAPK_HEADLESS").empty()) activate_running(instance);
        winapk::host::signal_launcher_ready();  // the running copy is what the user sees
        return 0;
    }
    if (!install_app(&b.options, b.options.data / "android")) return 1;
    // The installed APK, for the Java side's first moments: PresentedSdk reads
    // the app's targetSdkVersion and minSdkVersion from it when Build.VERSION
    // initializes, before any loader class could hand it over (upstream's
    // main.cpp exports it the same way). Without it every app was presented
    // API 29, below Play services' minSdk 31. Always this run's own APK: a
    // value inherited from the process that started this one (the broker
    // starting Play services for an app) names another app.
    SetEnvironmentVariableW(L"MACAPK_APK_PATH", b.options.apk.c_str());
    prepare_arm64(b.options, b.app_dir);
    if (!convert_dex(b.options, b.app_dir, &b.app_jar)) return 1;

    // Upstream runs the JVM with -Xss16m; give the creating thread as much.
    HANDLE t = CreateThread(nullptr, 64 * 1024 * 1024, boot_thread, &b, STACK_SIZE_PARAM_IS_A_RESERVATION,
                            nullptr);
    if (!t) return 1;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return b.result;
}
