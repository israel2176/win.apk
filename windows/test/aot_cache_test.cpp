// aot_cache_test.cpp -- the files of the arm64 translation (include/winapk/
// aot_files.h) under processes that race and processes that are killed.
//
//   winapk-aot-cache-test <scratch dir>
//
// Needs winapk-aot-test-dll.dll (test/aot_cache_dll.cpp) next to it: the
// "translation" the processes seal, share, install and load. Each check runs
// in real processes, as the apps' workers and loaders do:
//
//   1. a seal: a DLL still loads with one; cut short, zeros, a byte changed
//      or no seal at all, the check tells;
//   2. eight processes take, make, put and install the same cache entry at
//      once, each with different bytes, keeping what they load mapped: one
//      good entry, every app's DLL whole, no temporary file left; with hard
//      links and with copies (WINAPK_AOT_FAULT=nolink);
//   3. writers killed (TerminateProcess) at random moments, again and again:
//      every name points at a whole DLL or at none, a stamp is whole or the
//      old one, stale temporary files go, and the next run finishes;
//   4. a writer killed at each step of WINAPK_AOT_FAULT (half a seal, a cache
//      entry under its temporary name, half a stamp);
//   5. a cache entry cut short, empty, unsealed or changed is not taken but
//      deleted, and a good one takes its place;
//   6. eviction and replacement while another process has the DLL loaded:
//      that process runs on, and the entry goes once it is free; a take into
//      a name that is already the loaded entry's never writes into it;
//   7. the clock: an entry not used for 60 days goes, one used "in the
//      future" ages from now, and over the size cap the oldest use goes first;
//   8. stamps written over and over by four processes while another reads
//      them: never a partial one.

#include "winapk/aot_files.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using winapk::arm64::AotFile;

namespace {

int g_failures = 0;
fs::path g_exe, g_src;

void check(const char* what, bool ok, const std::string& detail = "") {
    std::printf("  %s %s%s%s\n", ok ? "ok  " : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

const char* name(AotFile f) {
    switch (f) {
        case AotFile::kSealed: return "sealed";
        case AotFile::kUnsealed: return "unsealed";
        case AotFile::kBroken: return "broken";
        case AotFile::kMissing: return "missing";
    }
    return "?";
}

// A fresh "linker output" at `p`, sealed: the test DLL with one byte of its
// data changed by `variant`, so different writers make different files.
bool make_dll(const fs::path& p, unsigned variant) {
    // A new file, as runtime_main.cpp makes the linker's: `p` may be another
    // name of the cache's entry.
    if (!winapk::arm64::aot_retire(p) || !CopyFileW(g_src.c_str(), p.c_str(), TRUE)) return false;
    if (variant != 0) {
        HANDLE f = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{}, at{};
        GetFileSizeEx(f, &size);
        at.QuadPart = size.QuadPart / 2;  // in the 8 MB of data
        DWORD n;
        const unsigned char b = static_cast<unsigned char>(variant);
        SetFilePointerEx(f, at, nullptr, FILE_BEGIN);
        WriteFile(f, &b, 1, &n, nullptr);
        CloseHandle(f);
    }
    return winapk::arm64::aot_seal(p);
}

// Loads `p` and calls it: 42, or -1 when it does not load. `keep`: leave it
// loaded (mapped) for the rest of the process.
int call_dll(const fs::path& p, bool keep = false) {
    HMODULE h = LoadLibraryW(p.c_str());
    if (!h) return -1;
    auto fn = reinterpret_cast<int (*)()>(reinterpret_cast<void*>(GetProcAddress(h, "winapk_aot_test_value")));
    const int v = fn ? fn() : -2;
    if (!keep) FreeLibrary(h);
    return v;
}

uintmax_t size_of(const fs::path& p) {
    std::error_code ec;
    const uintmax_t n = fs::file_size(p, ec);
    return ec ? 0 : n;
}

void set_size(const fs::path& p, uint64_t n) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(n);
    SetFilePointerEx(f, at, nullptr, FILE_BEGIN);
    SetEndOfFile(f);
    CloseHandle(f);
}

void set_age(const fs::path& p, std::chrono::seconds age) {
    std::error_code ec;
    fs::last_write_time(p, fs::file_time_type::clock::now() - age, ec);
}

size_t count_ext(const fs::path& dir, const wchar_t* ext) {
    std::error_code ec;
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir, ec)) n += e.path().extension() == ext;
    return n;
}

uint64_t file_index(const fs::path& p) {
    HANDLE f = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           0, nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = f != INVALID_HANDLE_VALUE && GetFileInformationByHandle(f, &info);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    return ok ? static_cast<uint64_t>(info.nFileIndexHigh) << 32 | info.nFileIndexLow : 0;
}

HANDLE spawn(const std::wstring& args) {
    std::wstring cmd = L"\"" + g_exe.wstring() + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

DWORD finish(HANDLE p, DWORD timeout_ms = 120000) {
    DWORD code = 1000;
    if (WaitForSingleObject(p, timeout_ms) != WAIT_OBJECT_0) TerminateProcess(p, 1001);
    WaitForSingleObject(p, INFINITE);
    GetExitCodeProcess(p, &code);
    CloseHandle(p);
    return code;
}

std::wstring q(const fs::path& p) { return L"\"" + p.wstring() + L"\""; }

// An app's files, as the worker lays them out.
struct App {
    fs::path dir, work, target, later, stamp;
    explicit App(const fs::path& d)
        : dir(d), work(d / ".aot-tmp"), target(d / "lib.so.aot.dll"), later(d / "lib.so.aot.dll.new"),
          stamp(d / "lib.so.aot.stamp") {
        std::error_code ec;
        fs::create_directories(work, ec);
    }
};

// ---- the child processes ----

// One worker's pass over the shared cache's entry, then the app's loader:
// take it from the cache or make it and put it there; put it in place (or
// beside, when the one in place is loaded); stamp; load what is in place.
// `variant` 0: a fresh variant each time. `keep`: the DLL in place stays
// loaded, as in a running app (which loads the one beside at its next start).
bool worker_pass(const fs::path& cached, const App& app, unsigned variant, bool keep, int round) {
    const fs::path dll = app.work / "lib.so.aot.dll";
    if (!winapk::arm64::aot_cache_take(cached, dll)) {
        if (!make_dll(dll, variant ? variant : 1 + static_cast<unsigned>(round % 200))) return false;
        winapk::arm64::aot_cache_put(dll, cached);
    }
    if (!winapk::arm64::aot_replace(dll, app.target, 3) && !winapk::arm64::aot_replace(dll, app.later, 3)) {
        return false;  // both in use: as runtime_main's install, a failure for the next pass
    }
    char line[64];
    std::snprintf(line, sizeof line, "full %d end\n", round);
    winapk::arm64::aot_write_file(app.stamp, line);
    // The loader: only a sealed DLL, and that one must load and run.
    for (const fs::path& p : {app.target, app.later}) {
        const AotFile f = winapk::arm64::aot_check_dll(p, false);
        if (f == AotFile::kMissing) continue;
        if (f != AotFile::kSealed || winapk::arm64::aot_check_dll(p, true) != AotFile::kSealed) return false;
        if (call_dll(p, keep && p == app.target) != 42) return false;
    }
    return true;
}

int child_main(int argc, wchar_t** argv) {
    const std::wstring mode = argv[1];
    if (mode == L"race" && argc == 6) {  // race <cache entry> <app dir> <variant> <rounds>
        App app(argv[3]);
        const unsigned variant = static_cast<unsigned>(std::wcstoul(argv[4], nullptr, 10));
        const int rounds = std::wcstol(argv[5], nullptr, 10);
        for (int r = 0; r < rounds; ++r) {
            if (!worker_pass(argv[2], app, variant, true, r)) return 10 + r;
        }
        return 0;
    }
    if (mode == L"writer" && argc == 4) {  // writer <cache entry> <app dir>: until killed
        App app(argv[3]);
        for (int r = 0;; ++r) {
            if (r % 3 == 2) winapk::arm64::aot_retire(argv[2]);  // so the next pass puts again
            worker_pass(argv[2], app, 0, false, r);
        }
    }
    if (mode == L"hold" && argc == 5) {  // hold <dll> <ready event> <go event>
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3]);
        HANDLE go = OpenEventW(SYNCHRONIZE, FALSE, argv[4]);
        HMODULE h = LoadLibraryW(argv[2]);
        SetEvent(ready);
        if (!h) return 2;
        WaitForSingleObject(go, 60000);
        auto fn = reinterpret_cast<int (*)()>(reinterpret_cast<void*>(GetProcAddress(h, "winapk_aot_test_value")));
        return fn && fn() == 42 ? 0 : 3;
    }
    if (mode == L"stamp" && argc == 5) {  // stamp <path> <id> <count>: the failures, in percent
        const int count = std::wcstol(argv[4], nullptr, 10);
        int failed = 0;
        for (int i = 0; i < count; ++i) {
            char line[96];
            std::snprintf(line, sizeof line, "full %ls %d 7 0 0 0123456789abcdef end\n", argv[3], i);
            failed += !winapk::arm64::aot_write_file(argv[2], line);
        }
        return failed * 100 / count;
    }
    if (mode == L"fault" && argc == 5) {  // fault <step> <cache entry> <app dir>: killed at that step
        App app(argv[4]);
        const fs::path dll = app.work / "lib.so.aot.dll";
        if (!make_dll(dll, 7)) return 2;  // kill:seal ends here
        if (!winapk::arm64::aot_cache_put(dll, argv[3])) return 3;  // kill:cache-put here
        winapk::arm64::aot_write_file(app.stamp, "full 99 end\n");  // kill:write here
        return 0;
    }
    return 100;
}

// ---- the checks ----

void test_seal(const fs::path& dir) {
    std::printf("1. the seal\n");
    const fs::path a = dir / "a.dll";
    check("made and sealed", make_dll(a, 0));
    check("sealed (cheap check)", winapk::arm64::aot_check_dll(a, false) == AotFile::kSealed);
    check("sealed (whole check)", winapk::arm64::aot_check_dll(a, true) == AotFile::kSealed);
    check("a sealed DLL loads and runs", call_dll(a) == 42);
    check("the test DLL itself: unsealed", winapk::arm64::aot_check_dll(g_src, true) == AotFile::kUnsealed);
    check("missing", winapk::arm64::aot_check_dll(dir / "none.dll", false) == AotFile::kMissing);
    const uintmax_t full = size_of(a);
    struct Case {
        const char* what;
        void (*spoil)(const fs::path&, uintmax_t);
        bool cheap_sees;  // the cheap check catches it too
    };
    const Case cases[] = {
        {"cut short by one byte", [](const fs::path& p, uintmax_t n) { set_size(p, n - 1); }, true},
        {"cut in half", [](const fs::path& p, uintmax_t n) { set_size(p, n / 2); }, true},
        {"empty", [](const fs::path& p, uintmax_t) { set_size(p, 0); }, true},
        {"zeros where the data never reached the disk",
         [](const fs::path& p, uintmax_t n) {
             set_size(p, 0);
             set_size(p, n);
         },
         true},
        {"one byte changed", [](const fs::path& p, uintmax_t n) {
             std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
             f.seekp(static_cast<std::streamoff>(n / 3));
             f.put('\x5a');
         },
         false},
        {"something appended after the seal", [](const fs::path& p, uintmax_t) {
             std::ofstream(p, std::ios::binary | std::ios::app) << "more";
         },
         true},
    };
    for (const Case& c : cases) {
        const fs::path b = dir / "b.dll";
        make_dll(b, 0);
        c.spoil(b, full);
        const AotFile cheap = winapk::arm64::aot_check_dll(b, false), whole = winapk::arm64::aot_check_dll(b, true);
        check((std::string(c.what) + ": refused by the whole check").c_str(), whole != AotFile::kSealed, name(whole));
        if (c.cheap_sees) {
            check((std::string(c.what) + ": and by the loader's").c_str(), cheap != AotFile::kSealed, name(cheap));
        }
    }
}

void test_race(const fs::path& dir, bool nolink) {
    std::printf("2. eight processes on one cache entry at once (%s)\n", nolink ? "copies" : "hard links");
    const fs::path cache = dir / "cache", cached = cache / "key.aot.dll";
    std::error_code ec;
    fs::create_directories(cache, ec);
    SetEnvironmentVariableW(L"WINAPK_AOT_FAULT", nolink ? L"nolink" : nullptr);
    std::vector<HANDLE> procs;
    for (int i = 0; i < 8; ++i) {
        procs.push_back(spawn(L"race " + q(cached) + L" " + q(dir / ("app" + std::to_string(i))) + L" " +
                              std::to_wstring(i + 1) + L" 4"));
    }
    std::string codes;
    bool all = true;
    for (HANDLE p : procs) {
        const DWORD c = p ? finish(p) : 1000;
        codes += std::to_string(c) + " ";
        all = all && c == 0;
    }
    SetEnvironmentVariableW(L"WINAPK_AOT_FAULT", nullptr);
    check("every process took or made a whole DLL, put it in place, loaded and ran it", all, codes);
    check("one good entry in the cache", count_ext(cache, L".dll") == 1 &&
                                             winapk::arm64::aot_check_dll(cached, true) == AotFile::kSealed);
    check("no temporary file left in the cache", count_ext(cache, L".tmp") == 0);
    size_t whole = 0, tmps = 0;
    for (int i = 0; i < 8; ++i) {
        App app(dir / ("app" + std::to_string(i)));
        whole += winapk::arm64::aot_check_dll(app.target, true) == AotFile::kSealed && call_dll(app.target) == 42;
        tmps += count_ext(app.dir, L".tmp");
    }
    check("every app's DLL whole and running", whole == 8, std::to_string(whole) + " of 8");
    check("no temporary file left beside the apps' DLLs", tmps == 0, std::to_string(tmps));
}

// After a kill: what each name points at must be whole, or nothing.
bool consistent(const fs::path& cached, const App& app, std::string* why) {
    for (const fs::path& p : {cached, app.target, app.later}) {
        const AotFile f = winapk::arm64::aot_check_dll(p, true);
        if (f == AotFile::kMissing) continue;
        if (f != AotFile::kSealed) {
            *why = p.filename().u8string() + " is " + name(f);
            return false;
        }
        if (call_dll(p) != 42) {
            *why = p.filename().u8string() + " does not load";
            return false;
        }
    }
    std::ifstream in(app.stamp, std::ios::binary);
    std::string stamp((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.is_open() && (stamp.size() < 4 || stamp.compare(stamp.size() - 4, 4, "end\n") != 0)) {
        *why = "a partial stamp: \"" + stamp + "\"";
        return false;
    }
    return true;
}

void test_kills(const fs::path& dir, bool nolink) {
    std::printf("3. writers killed at random moments (%s)\n", nolink ? "copies" : "hard links");
    const fs::path cache = dir / "cache", cached = cache / "key.aot.dll";
    std::error_code ec;
    fs::create_directories(cache, ec);
    App a(dir / "appA"), b(dir / "appB");
    SetEnvironmentVariableW(L"WINAPK_AOT_FAULT", nolink ? L"nolink" : nullptr);
    std::mt19937 rng(nolink ? 2 : 1);
    int bad = 0;
    std::string first;
    const int kRounds = 40;
    for (int i = 0; i < kRounds; ++i) {
        HANDLE pa = spawn(L"writer " + q(cached) + L" " + q(a.dir));
        HANDLE pb = spawn(L"writer " + q(cached) + L" " + q(b.dir));
        Sleep(20 + rng() % 400);
        TerminateProcess(pa, 9);
        Sleep(rng() % 20);
        TerminateProcess(pb, 9);
        finish(pa);
        finish(pb);
        std::string why;
        for (const App* app : {&a, &b}) {
            if (!consistent(cached, *app, &why)) {
                ++bad;
                if (first.empty()) first = "round " + std::to_string(i) + ": " + why;
            }
        }
    }
    check("after every kill each name is a whole DLL or none, and each stamp whole", bad == 0, first);
    const size_t left = count_ext(cache, L".tmp") + count_ext(a.dir, L".tmp") + count_ext(b.dir, L".tmp");
    std::printf("  (%zu temporary files left by the killed writers)\n", left);
    // An hour later, the next pass deletes them.
    for (const fs::path& d : {cache, a.dir, b.dir}) {
        for (const auto& e : fs::directory_iterator(d, ec)) {
            if (e.path().extension() == L".tmp") set_age(e.path(), std::chrono::hours(2));
        }
    }
    winapk::arm64::aot_cache_trim(cache, uintmax_t{1} << 40, 60);
    winapk::arm64::aot_remove_stale_temps(a.dir);
    winapk::arm64::aot_remove_stale_temps(b.dir);
    const size_t still = count_ext(cache, L".tmp") + count_ext(a.dir, L".tmp") + count_ext(b.dir, L".tmp");
    check("stale temporary files deleted by the next pass", still == 0, std::to_string(still) + " left");
    const DWORD code = finish(spawn(L"race " + q(cached) + L" " + q(a.dir) + L" 0 2"));
    SetEnvironmentVariableW(L"WINAPK_AOT_FAULT", nullptr);
    check("the next run finishes with a whole DLL", code == 0, "exit code " + std::to_string(code));
}

void test_fault_points(const fs::path& dir) {
    std::printf("4. killed at each step\n");
    const fs::path cache = dir / "cache", cached = cache / "key.aot.dll";
    std::error_code ec;
    fs::create_directories(cache, ec);
    App app(dir / "app");
    winapk::arm64::aot_write_file(app.stamp, "full 1 end\n");
    auto run = [&](const wchar_t* spec) {
        SetEnvironmentVariableW(L"WINAPK_AOT_FAULT", spec);
        const DWORD code = finish(spawn(L"fault x " + q(cached) + L" " + q(app.dir)));
        SetEnvironmentVariableW(L"WINAPK_AOT_FAULT", nullptr);
        return code;
    };
    // Half a seal: a file that is no translation.
    check("kill:seal ends the writer there", run(L"kill:seal") == 99);
    const AotFile half = winapk::arm64::aot_check_dll(app.work / "lib.so.aot.dll", false);
    check("half a seal is no seal", half != AotFile::kSealed, name(half));
    check("nothing reached the cache", !fs::exists(cached, ec));
    // A cache entry under its temporary name, never under the real one.
    for (const wchar_t* spec : {L"kill:cache-put", L"nolink,kill:cache-put"}) {
        check("kill:cache-put ends the writer there", run(spec) == 99);
        check("the entry is not there half made", !fs::exists(cached, ec));
        check("its temporary file is", count_ext(cache, L".tmp") >= 1);
    }
    winapk::arm64::aot_cache_trim(cache, uintmax_t{1} << 40, 60);
    check("a fresh one is left alone (its writer may still be at it)", count_ext(cache, L".tmp") >= 1);
    for (const auto& e : fs::directory_iterator(cache, ec)) set_age(e.path(), std::chrono::hours(2));
    winapk::arm64::aot_cache_trim(cache, uintmax_t{1} << 40, 60);
    check("an hour later it is deleted", count_ext(cache, L".tmp") == 0);
    // Half a stamp: under its temporary name; the old stamp stays whole.
    check("kill:write ends the writer there", run(L"kill:write") == 99);
    std::ifstream in(app.stamp, std::ios::binary);
    std::string stamp((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    check("the stamp is still the old one, whole", stamp == "full 1 end\n", stamp);
    check("the cache entry the writer put is whole",
          winapk::arm64::aot_check_dll(cached, true) == AotFile::kSealed && call_dll(cached) == 42);
    check("without a fault, the writer finishes", run(nullptr) == 0);
    check("and the stamp is its", [&] {
        std::ifstream s(app.stamp, std::ios::binary);
        std::string t((std::istreambuf_iterator<char>(s)), std::istreambuf_iterator<char>());
        return t == "full 99 end\n";
    }());
}

void test_corrupt(const fs::path& dir) {
    std::printf("5. a broken cache entry\n");
    const fs::path cache = dir / "cache", cached = cache / "key.aot.dll";
    std::error_code ec;
    fs::create_directories(cache, ec);
    App app(dir / "app");
    const fs::path to = app.work / "lib.so.aot.dll";
    struct Case {
        const char* what;
        void (*spoil)(const fs::path&);
    };
    const Case cases[] = {
        {"cut in half", [](const fs::path& p) { set_size(p, size_of(p) / 2); }},
        {"empty", [](const fs::path& p) { set_size(p, 0); }},
        {"unsealed", [](const fs::path& p) { CopyFileW(g_src.c_str(), p.c_str(), FALSE); }},
        {"one byte changed", [](const fs::path& p) {
             std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
             f.seekp(1000);
             f.put('\x77');
         }},
    };
    for (const Case& c : cases) {
        make_dll(cached, 3);
        c.spoil(cached);
        const bool took = winapk::arm64::aot_cache_take(cached, to);
        check((std::string(c.what) + ": not taken").c_str(), !took && !fs::exists(to, ec));
        check((std::string(c.what) + ": deleted").c_str(), !fs::exists(cached, ec) && count_ext(cache, L".tmp") == 0);
        const fs::path fresh = app.work / "fresh.dll";
        make_dll(fresh, 4);
        check((std::string(c.what) + ": a good one put in its place").c_str(),
              winapk::arm64::aot_cache_put(fresh, cached) && winapk::arm64::aot_cache_take(cached, to) &&
                  call_dll(to) == 42);
        DeleteFileW(to.c_str());
        DeleteFileW(cached.c_str());
    }
}

void test_in_use(const fs::path& dir) {
    std::printf("6. eviction and replacement of a DLL another process has loaded\n");
    const fs::path cache = dir / "cache", cached = cache / "key.aot.dll";
    std::error_code ec;
    fs::create_directories(cache, ec);
    App app(dir / "app");
    const fs::path dll = app.work / "lib.so.aot.dll";
    make_dll(dll, 5);
    winapk::arm64::aot_cache_put(dll, cached);
    winapk::arm64::aot_replace(dll, app.target);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, L"WinApkAotTestReady");
    HANDLE go = CreateEventW(nullptr, TRUE, FALSE, L"WinApkAotTestGo");
    // Loaded through the cache's own name, then through the app's.
    for (const fs::path& held : {cached, app.target}) {
        ResetEvent(ready);
        ResetEvent(go);
        const bool by_cache = held == cached;
        winapk::arm64::aot_cache_take(cached, app.work / "x.dll");  // a hard link: both names, one file
        winapk::arm64::aot_replace(app.work / "x.dll", app.target);
        HANDLE holder = spawn(L"hold " + q(held) + L" WinApkAotTestReady WinApkAotTestGo");
        WaitForSingleObject(ready, 30000);
        const uint64_t before = file_index(cached);
        // A different good DLL: put keeps the entry that is there.
        const fs::path other = app.work / "other.dll";
        make_dll(other, 6);
        check("put leaves a good entry in place", winapk::arm64::aot_cache_put(other, cached) &&
                                                       file_index(cached) == before);
        // Over the size cap: the trim tries to delete it.
        winapk::arm64::aot_cache_trim(cache, 0, 60);
        const bool gone = !fs::exists(cached, ec);
        std::printf("  (loaded through the %s's name, the trim %s the cache's name)\n", by_cache ? "cache" : "app",
                    gone ? "deleted" : "could not delete");
        if (by_cache) {
            // A take killed before its install left the work folder's name
            // as a second name of this entry; the next take goes into it.
            const fs::path stale = app.work / "stale.dll";
            DeleteFileW(stale.c_str());
            CreateHardLinkW(stale.c_str(), cached.c_str(), nullptr);
            const bool took = winapk::arm64::aot_cache_take(cached, stale);
            const AotFile entry = winapk::arm64::aot_check_dll(cached, true);
            check("a take into an old second name of the loaded entry leaves the entry whole",
                  entry == AotFile::kSealed, name(entry));
            check("and takes it", took && winapk::arm64::aot_check_dll(stale, true) == AotFile::kSealed);
            const bool replaced = winapk::arm64::aot_replace(other, cached, 2);
            std::printf("  (renaming another file over it: %s)\n", replaced ? "done" : "refused, as Windows does");
            check("a refused rename leaves the loaded file's name in place", replaced || fs::exists(cached, ec));
        }
        SetEvent(go);
        check("the process that has it loaded runs on", finish(holder) == 0);
        if (!fs::exists(cached, ec)) {
            make_dll(dll, 5);
            winapk::arm64::aot_cache_put(dll, cached);
        }
    }
    winapk::arm64::aot_cache_trim(cache, 0, 60);
    check("once nothing has it loaded, the trim deletes it", !fs::exists(cached, ec));
    check("and the app's own name still runs it", call_dll(app.target) == 42);
    CloseHandle(ready);
    CloseHandle(go);
}

void test_clock(const fs::path& dir) {
    std::printf("7. the clock\n");
    const fs::path cache = dir / "cache";
    std::error_code ec;
    fs::create_directories(cache, ec);
    const fs::path old = cache / "old.aot.dll", future = cache / "future.aot.dll", now = cache / "now.aot.dll";
    for (const fs::path& p : {old, future, now}) make_dll(p, 8);
    set_age(old, std::chrono::hours(24 * 61));
    set_age(future, -std::chrono::hours(24 * 365 * 10));
    winapk::arm64::aot_cache_trim(cache, uintmax_t{1} << 40, 60);
    check("not used for 61 days: deleted", !fs::exists(old, ec));
    check("used now: kept", fs::exists(now, ec));
    const auto t = fs::last_write_time(future, ec);
    check("used \"in ten years\": kept, and aging from now",
          fs::exists(future, ec) && t <= fs::file_time_type::clock::now() + std::chrono::minutes(5));
    set_age(future, std::chrono::hours(2));
    winapk::arm64::aot_cache_trim(cache, size_of(now), 60);
    check("over the cap: the least recently used goes first", !fs::exists(future, ec) && fs::exists(now, ec));
}

void test_stamps(const fs::path& dir) {
    std::printf("8. stamps written by four processes while another reads them\n");
    const fs::path stamp = dir / "lib.so.aot.stamp";
    std::vector<HANDLE> writers;
    for (int i = 0; i < 4; ++i) writers.push_back(spawn(L"stamp " + q(stamp) + L" w" + std::to_wstring(i) + L" 300"));
    int reads = 0, partial = 0;
    std::string seen;
    for (;;) {
        bool running = false;
        for (HANDLE w : writers) running = running || WaitForSingleObject(w, 0) == WAIT_TIMEOUT;
        // As the loader and the worker read it (aot_read_file).
        const std::string s = winapk::arm64::aot_read_file(stamp);
        if (!s.empty()) {
            ++reads;
            if (s.rfind("full w", 0) != 0 || s.size() < 4 || s.compare(s.size() - 4, 4, "end\n") != 0) {
                if (!partial++) seen = s;
            }
        }
        if (!running) break;
        Sleep(1);
    }
    std::string failed;
    bool most = true;
    for (HANDLE w : writers) {
        const DWORD pct = finish(w);
        failed += std::to_string(pct) + "% ";
        most = most && pct <= 10;
    }
    check("never a partial stamp", partial == 0, std::to_string(partial) + " of " + std::to_string(reads) +
                                                     " reads, e.g. \"" + seen + "\"");
    check("the writes get through a reader (retried while it has the file open)", most, failed);
    check("no temporary file left", count_ext(dir, L".tmp") == 0);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    wchar_t self[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, self, MAX_PATH * 4);
    g_exe = self;
    g_src = g_exe.parent_path() / "winapk-aot-test-dll.dll";
    if (argc >= 3) return child_main(argc, argv);  // one of the processes the checks start
    if (argc != 2) {
        std::fprintf(stderr, "usage: winapk-aot-cache-test <scratch dir>\n");
        return 2;
    }
    std::error_code ec;
    if (!fs::exists(g_src, ec)) {
        std::fprintf(stderr, "no %s\n", g_src.u8string().c_str());
        return 2;
    }
    const fs::path root = fs::path(argv[1]) / "aot-cache-test";
    fs::remove_all(root, ec);
    int n = 0;
    auto fresh = [&] {
        const fs::path d = root / std::to_string(++n);
        fs::create_directories(d, ec);
        return d;
    };
    test_seal(fresh());
    test_race(fresh(), false);
    test_race(fresh(), true);
    test_kills(fresh(), false);
    test_kills(fresh(), true);
    test_fault_points(fresh());
    test_corrupt(fresh());
    test_in_use(fresh());
    test_clock(fresh());
    test_stamps(fresh());
    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures;
}
