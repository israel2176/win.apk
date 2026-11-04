// A stand-in for clang.exe: "-c -o <out> <src>" waits FAKECLANG_MS ms and writes <out>; "-shared -o <dll> objs..."
// writes <dll>. Logs "start|end <pid> <ms>" to FAKECLANG_LOG. Fails (exit 1) when FAKECLANG_FAIL is in <src>.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
int wmain(int argc, wchar_t** argv) {
    std::wstring out, src; bool shared = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"-o" && i + 1 < argc) out = argv[++i];
        else if (a == L"-shared") shared = true;
        else if (a[0] != L'-') src = a;
    }
    wchar_t buf[4096];
    auto log = [&](const char* what) {
        if (GetEnvironmentVariableW(L"FAKECLANG_LOG", buf, 4096)) {
            FILE* f = _wfopen(buf, L"ab");
            if (f) { std::fprintf(f, "%s %lu %llu\n", what, GetCurrentProcessId(), (unsigned long long)GetTickCount64()); std::fclose(f); }
        }
    };
    if (!shared) {
        log("start");
        DWORD ms = 300;
        if (GetEnvironmentVariableW(L"FAKECLANG_MS", buf, 4096)) ms = wcstoul(buf, nullptr, 10);
        Sleep(ms);
        if (GetEnvironmentVariableW(L"FAKECLANG_FAIL", buf, 4096) && src.find(buf) != std::wstring::npos) { log("fail"); return 1; }
        log("end");
    }
    FILE* f = _wfopen(out.c_str(), L"wb");
    if (!f) return 1;
    std::fprintf(f, "object of %ls\n", src.c_str());
    std::fclose(f);
    return 0;
}
