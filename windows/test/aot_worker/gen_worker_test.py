"""Builds a test program from host/runtime_main.cpp's background translation
worker alone (the section "arm64 translation, in the background" up to
prepare_arm64, and the helpers it uses), for run_aot_worker_wine.sh.

usage: python3 gen_worker_test.py <runtime_main.cpp> <out.cpp>

The program: --aot-translate <dir> [--cache <dir>] [--delay <s>] as
winapk-runtime's, <lib.so> alone to print aot_needs_work (1 or 0), and
--check <dll> to print what aot_check_dll finds (sealed, unsealed, broken,
missing). Link it with src/arm64/aot_files.cpp.
"""
import re, sys
src = open(sys.argv[1]).read()
def func(name_regex):
    m = re.search(name_regex, src, re.M)
    start = m.start()
    # to the matching closing brace at column 0
    end = src.index("\n}\n", start) + 3
    return src[start:end]
parts = [func(r"^std::string utf8\(const std::wstring& w\)"), func(r"^std::wstring env_var\("), func(r"^fs::path exe_dir\(\)"),
         func(r"^std::wstring quoted\(const std::wstring& a\)"), "std::wstring quoted(const fs::path& p) { return quoted(p.wstring()); }\n",
         func(r"^int run_process\("), func(r"^std::string file_id\(")]
a = src.index("// ---- arm64 translation, in the background"); b = src.index("// Step 1b, before the app's libraries load")
out = '''// Generated from host/runtime_main.cpp: the background translation worker alone, for a test under Wine.
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
#include <windows.h>
#include <shellapi.h>
#include "winapk/aot_files.h"
#include "winapk/arm64_aot.h"
namespace winapk::arm64 { size_t aot_profile_flush() { return 0; } size_t aot_load_pending() { return 0; } }
namespace fs = std::filesystem;
namespace {
''' + "\n".join(parts) + src[a:b] + '''
}  // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && std::wstring(argv[1]) == L"--aot-translate") {
        fs::path cache; DWORD delay_s = 0;
        for (int i = 3; i + 1 < argc; i += 2) {
            if (std::wstring(argv[i]) == L"--cache") cache = argv[i + 1];
            else if (std::wstring(argv[i]) == L"--delay") delay_s = static_cast<DWORD>(std::wcstoul(argv[i + 1], nullptr, 10));
        }
        return aot_translate_main(argv[2], cache, delay_s);
    }
    if (argc == 2) { std::printf("%d\\n", aot_needs_work(argv[1]) ? 1 : 0); return 0; }
    if (argc == 3 && std::wstring(argv[1]) == L"--check") {
        static const char* const names[] = {"sealed", "unsealed", "broken", "missing"};
        std::printf("%s\\n", names[static_cast<int>(winapk::arm64::aot_check_dll(argv[2], true))]);
        return 0;
    }
    return 2;
}
'''
open(sys.argv[2], "w").write(out)
