// binder_wire.h -- what cross-process Binder puts on a named pipe (host/binder_win.cpp), and the names of
// the pipes, for the two programs that speak it: winapk-runtime (every app and the system broker) and the
// launcher, which tells the broker of an install or a removal (package_events.h).
//
// A frame is its length (uint32, not counting itself), a Head, data_len bytes of parcel data, n_off int32
// offsets, then n_obj objects: an int32 kind and, for a binder, its number (uint32) and the endpoint's pipe
// name (uint32 length in UTF-16 units, then the units); for a file, the handle (uint64) already duplicated
// into the receiver. A process serves \\.\pipe\winapk-<user SID>-s<session>-b-<pid>-<random>; a published
// name (the broker's) is \\.\pipe\winapk-<user SID>-s<session>-<name> and answers kGetRoot only.

#pragma once

#include <cstdint>
#include <string>

#include <windows.h>
#include <sddl.h>

namespace winapk::binder {

constexpr uint32_t kMagic = 0x4e424157;  // 'WABN'
constexpr uint32_t kMaxFrame = 64u << 20;
enum : uint32_t { kTransact = 1, kReply = 2, kGetRoot = 3, kRoot = 4 };
// MacApkBinderTransport's object kinds.
enum : int32_t { kKindNull = 0, kKindBinder = 1, kKindPfd = 2, kKindFd = 3 };
constexpr int32_t kFlagOneway = 1;  // IBinder.FLAG_ONEWAY

struct Head {
    uint32_t magic, type, code, flags, target;
    int32_t status;
    uint32_t data_len, n_off, n_obj;
};

// The user a process token stands for, as a string SID; empty when it cannot be read.
inline std::wstring string_sid_of(HANDLE token) {
    DWORD n = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &n);
    if (n == 0) return {};
    std::wstring out;
    void* buf = HeapAlloc(GetProcessHeap(), 0, n);
    if (!buf) return {};
    wchar_t* s = nullptr;
    if (GetTokenInformation(token, TokenUser, buf, n, &n) &&
        ConvertSidToStringSidW(static_cast<TOKEN_USER*>(buf)->User.Sid, &s)) {
        out = s;
        LocalFree(s);
    }
    HeapFree(GetProcessHeap(), 0, buf);
    return out;
}

// The user `process` runs as; empty when it cannot be read.
inline std::wstring process_sid(HANDLE process) {
    HANDLE t = nullptr;
    if (!process || !OpenProcessToken(process, TOKEN_QUERY, &t)) return {};
    std::wstring s = string_sid_of(t);
    CloseHandle(t);
    return s;
}

// The program `process` runs; empty when it cannot be read.
inline std::wstring process_image(HANDLE process) {
    std::wstring buf(32768, L'\0');
    DWORD n = static_cast<DWORD>(buf.size());
    if (!process || !QueryFullProcessImageNameW(process, 0, buf.data(), &n)) return {};
    buf.resize(n);
    return buf;
}

inline bool same_path(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// The pipe-name prefix of this user's session. Pipe names are global, so the user and session in the name
// keep two users' runtimes apart; the DACL and the peer checks are what stop one from posing as the other.
inline std::wstring pipe_prefix() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return L"\\\\.\\pipe\\winapk-" + process_sid(GetCurrentProcess()) + L"-s" + std::to_wstring(session) + L"-";
}

// The launcher of the install a runtime is part of: <root>\Win.apk.exe for <root>\bin\winapk-runtime.exe
// (stage_runtime.sh, the installer). Empty for an image with no folder above its own.
inline std::wstring launcher_image_for(const std::wstring& runtime_image) {
    const size_t bin = runtime_image.find_last_of(L"\\/");
    if (bin == std::wstring::npos || bin == 0) return {};
    const size_t root = runtime_image.find_last_of(L"\\/", bin - 1);
    if (root == std::wstring::npos) return {};
    return runtime_image.substr(0, root + 1) + L"Win.apk.exe";
}

}  // namespace winapk::binder
