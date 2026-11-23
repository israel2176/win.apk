// package_events.h -- an install, an update or a removal, told to the system broker at its commit.
//
// With "Use Google services" on, the system broker (winapk-runtime --system,
// runtime/java/com/macapk/loader/SystemServer.java) plays system_server. On Android PackageManagerService
// learns a package when it is installed and commits it; ActivityManager then broadcasts PACKAGE_ADDED (and
// PACKAGE_REPLACED, MY_PACKAGE_REPLACED for an update) or PACKAGE_REMOVED to the apps that listen. Here
// the commit is the launcher's (install_into_library, remove_app; its install helper verbs are the same
// program), and once the registry (installed_apps.json) is written it calls tell_broker, which waits for
// the broker to have taken the package in: an app started right after is known to it. When no broker
// runs, nothing happens: a broker reads the registry when it starts.
//
// The call is SystemServer.TX_PACKAGE_CHANGED on the broker's published object, over the same pipes as
// every other Binder call (binder_wire.h): (op, package, APK, data removed) -> no exception. The broker
// takes it only from the launcher, by what Windows says the calling process runs (WinProcesses.isLauncher);
// the launcher in turn talks only to a broker that runs this install's winapk-runtime.exe as this user.
// Windows calls only, no lock, no JNI: winapk-binder-test runs the same code as the launcher.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include "binder_wire.h"

namespace winapk::package_events {

// SystemServer.PACKAGE_ADDED, PACKAGE_REPLACED, PACKAGE_REMOVED.
enum Change : int32_t { kAdded = 0, kReplaced = 1, kRemoved = 2 };
// SystemServer.TX_PACKAGE_CHANGED: IBinder.FIRST_CALL_TRANSACTION + 25.
constexpr uint32_t kTransaction = 1 + 25;
constexpr const wchar_t* kDescriptor = L"android.app.IMacApkSystem";  // SystemServer.DESCRIPTOR
constexpr const wchar_t* kBrokerName = L"org.macapk.system";         // SystemServer.DEFAULT_NAME

enum class Told {
    kNoBroker,  // nothing serves the broker's name: nobody to tell
    kDone,      // the broker took the change in
    kRefused,   // the broker answered with an exception (not the launcher, or a package it cannot read)
    kFailed,    // the pipe broke or timed out, or its server is not this install's runtime
};

// AOSP Parcel: an int32, little endian.
inline void put_int(std::vector<uint8_t>& b, int32_t v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof v);
}

// AOSP Parcel::writeString16: the length in UTF-16 units, the units and a NUL, padded to 4 bytes.
inline void put_string(std::vector<uint8_t>& b, const std::wstring& s) {
    put_int(b, static_cast<int32_t>(s.size()));
    const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    const size_t at = b.size();
    b.resize(at + ((bytes + 3) & ~size_t{3}), 0);
    std::memcpy(b.data() + at, s.c_str(), s.size() * sizeof(wchar_t));
}

// The parcel of one TX_PACKAGE_CHANGED.
inline std::vector<uint8_t> change_parcel(Change change, const std::wstring& package, const std::wstring& apk,
                                          bool data_removed) {
    std::vector<uint8_t> d;
    put_string(d, kDescriptor);  // writeInterfaceToken
    put_int(d, change);
    put_string(d, package);
    put_string(d, apk);
    put_int(d, data_removed ? 1 : 0);
    return d;
}

namespace detail {

// A connection to a pipe served by `server_image` running as this user; its server's pid in *pid.
inline HANDLE open_pipe(const std::wstring& name, const std::wstring& server_image, DWORD* pid, bool* missing) {
    *missing = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            if (e == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(name.c_str(), 500);
                continue;
            }
            *missing = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
        ULONG server = 0;
        HANDLE p = GetNamedPipeServerProcessId(h, &server)
                       ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, server)
                       : nullptr;
        const bool trusted = p && binder::process_sid(p) == binder::process_sid(GetCurrentProcess()) &&
                             binder::same_path(binder::process_image(p), server_image);
        if (p) CloseHandle(p);
        if (!trusted) {
            CloseHandle(h);
            return INVALID_HANDLE_VALUE;
        }
        *pid = server;
        return h;
    }
    return INVALID_HANDLE_VALUE;
}

// Reads or writes all of `len` before `deadline` (GetTickCount64).
inline bool io(HANDLE h, bool write, void* buf, size_t len, ULONGLONG deadline) {
    auto* p = static_cast<uint8_t*>(buf);
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return false;
    bool ok = true;
    while (len && ok) {
        OVERLAPPED ov{};
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD n = 0;
        const BOOL done = write ? WriteFile(h, p, static_cast<DWORD>(len), &n, &ov)
                                : ReadFile(h, p, static_cast<DWORD>(len), &n, &ov);
        if (!done) {
            if (GetLastError() != ERROR_IO_PENDING) { ok = false; break; }
            const ULONGLONG now = GetTickCount64();
            const DWORD wait = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
            if (WaitForSingleObject(ev, wait) != WAIT_OBJECT_0) {
                CancelIoEx(h, &ov);
                GetOverlappedResult(h, &ov, &n, TRUE);
                ok = false;
                break;
            }
            if (!GetOverlappedResult(h, &ov, &n, FALSE)) { ok = false; break; }
        }
        if (n == 0) { ok = false; break; }
        p += n;
        len -= n;
    }
    CloseHandle(ev);
    return ok;
}

// One request and its answer: the answer's head, data and (for kRoot) its first object.
inline bool exchange(HANDLE h, uint32_t type, uint32_t code, uint32_t target, const std::vector<uint8_t>& data,
                     ULONGLONG deadline, binder::Head* head, std::vector<uint8_t>* out_data,
                     std::wstring* endpoint, uint32_t* id) {
    binder::Head req{};
    req.magic = binder::kMagic;
    req.type = type;
    req.code = code;
    req.target = target;
    req.data_len = static_cast<uint32_t>(data.size());
    std::vector<uint8_t> frame(sizeof(uint32_t) + sizeof req);
    const uint32_t len = static_cast<uint32_t>(sizeof req + data.size());
    std::memcpy(frame.data(), &len, sizeof len);
    std::memcpy(frame.data() + sizeof len, &req, sizeof req);
    frame.insert(frame.end(), data.begin(), data.end());
    if (!io(h, true, frame.data(), frame.size(), deadline)) return false;
    uint32_t got = 0;
    if (!io(h, false, &got, sizeof got, deadline) || got < sizeof(binder::Head) || got > binder::kMaxFrame)
        return false;
    std::vector<uint8_t> in(got);
    if (!io(h, false, in.data(), in.size(), deadline)) return false;
    std::memcpy(head, in.data(), sizeof *head);
    if (head->magic != binder::kMagic || head->data_len > got - sizeof *head) return false;
    size_t at = sizeof *head;
    out_data->assign(in.begin() + static_cast<std::ptrdiff_t>(at),
                     in.begin() + static_cast<std::ptrdiff_t>(at + head->data_len));
    at += head->data_len + static_cast<size_t>(head->n_off) * sizeof(int32_t);
    if (endpoint && head->n_obj >= 1) {
        int32_t kind = 0;
        uint32_t n = 0;
        if (at + sizeof kind > in.size()) return false;
        std::memcpy(&kind, in.data() + at, sizeof kind);
        at += sizeof kind;
        if (kind != binder::kKindBinder || at + 2 * sizeof(uint32_t) > in.size()) return false;
        std::memcpy(id, in.data() + at, sizeof *id);
        std::memcpy(&n, in.data() + at + sizeof *id, sizeof n);
        at += 2 * sizeof(uint32_t);
        if (n > 1024 || at + n * sizeof(wchar_t) > in.size()) return false;
        endpoint->assign(reinterpret_cast<const wchar_t*>(in.data() + at), n);
    }
    return true;
}

}  // namespace detail

// Tells the broker of this user's session (served by `server_image`, the install's winapk-runtime.exe) that
// `package` was added, replaced or removed, with the APK the registry now names for it (a removal: the one
// it named). Waits for the answer, at most `timeout_ms`.
inline Told tell_broker(Change change, const std::wstring& package, const std::wstring& apk, bool data_removed,
                        const std::wstring& server_image, const std::wstring& name = kBrokerName,
                        DWORD timeout_ms = 20000) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    const std::wstring prefix = binder::pipe_prefix();
    DWORD broker = 0;
    bool missing = false;
    HANDLE pub = detail::open_pipe(prefix + name, server_image, &broker, &missing);
    if (pub == INVALID_HANDLE_VALUE) return missing ? Told::kNoBroker : Told::kFailed;
    binder::Head head{};
    std::vector<uint8_t> data;
    std::wstring endpoint;
    uint32_t id = 0;
    const bool rooted = detail::exchange(pub, binder::kGetRoot, 0, 0, {}, deadline, &head, &data, &endpoint, &id) &&
                        head.type == binder::kRoot;
    CloseHandle(pub);
    // The published object lives on the broker's own endpoint, under this user's prefix.
    if (!rooted || endpoint.compare(0, prefix.size(), prefix) != 0) return Told::kFailed;
    DWORD server = 0;
    HANDLE ep = detail::open_pipe(endpoint, server_image, &server, &missing);
    if (ep == INVALID_HANDLE_VALUE) return missing ? Told::kNoBroker : Told::kFailed;
    if (server != broker) {
        CloseHandle(ep);
        return Told::kFailed;
    }
    const bool answered = detail::exchange(ep, binder::kTransact, kTransaction, id,
                                           change_parcel(change, package, apk, data_removed), deadline, &head,
                                           &data, nullptr, nullptr);
    CloseHandle(ep);
    if (!answered || head.type != binder::kReply || head.status != 0 || data.size() < sizeof(int32_t))
        return Told::kFailed;
    int32_t exception = 0;
    std::memcpy(&exception, data.data(), sizeof exception);
    return exception == 0 ? Told::kDone : Told::kRefused;
}

}  // namespace winapk::package_events
