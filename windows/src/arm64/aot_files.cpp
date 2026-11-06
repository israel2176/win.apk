// aot_files.cpp -- the files of the arm64 translation, safe against crashes and
// other processes. See include/winapk/aot_files.h.

#include "winapk/aot_files.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <utility>
#include <vector>

#include <windows.h>

namespace winapk::arm64 {

namespace fs = std::filesystem;

namespace {

constexpr char kSealMagic[16] = {'w', 'i', 'n', 'a', 'p', 'k', '-', 'a', 'o', 't', '-', 's', 'e', 'a', 'l', '1'};
struct Seal {
    char magic[16];
    uint64_t length;  // the bytes before the seal
    uint64_t hash;    // hash_file of them
};
static_assert(sizeof(Seal) == kAotSealBytes, "the seal is 32 bytes");

// The hash of a file's first `length` bytes, a chunk at a time (every chunk
// but the last a multiple of 8 bytes, so the result does not depend on how
// the reads fall).
constexpr DWORD kChunk = 1 << 20;
bool hash_file(HANDLE f, uint64_t length, uint64_t* out) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(f, zero, nullptr, FILE_BEGIN)) return false;
    std::vector<uint8_t> buf(kChunk);
    uint64_t h = 0x77696e61706b2d31ull ^ length;
    for (uint64_t done = 0; done < length;) {
        const DWORD want = static_cast<DWORD>(std::min<uint64_t>(kChunk, length - done));
        DWORD got = 0;
        if (!ReadFile(f, buf.data(), want, &got, nullptr) || got != want) return false;
        h = aot_content_hash(buf.data(), got, h);
        done += got;
    }
    *out = h;
    return true;
}

bool write_all(HANDLE f, const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (n > 0) {
        DWORD wrote = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(n, kChunk));
        if (!WriteFile(f, p, want, &wrote, nullptr) || wrote == 0) return false;
        p += wrote;
        n -= wrote;
    }
    return true;
}

const std::string& fault_spec() {
    static const std::string spec = [] {
        char buf[512] = {};
        const DWORD n = GetEnvironmentVariableA("WINAPK_AOT_FAULT", buf, sizeof buf);
        return n > 0 && n < sizeof buf ? "," + std::string(buf, n) + "," : std::string();
    }();
    return spec;
}

std::string narrow(const fs::path& p) { return p.filename().u8string(); }

}  // namespace

uint64_t aot_content_hash(const void* data, size_t n, uint64_t h) {
    const auto* p = static_cast<const uint8_t*>(data);
    auto mix = [&h](uint64_t w) {
        h ^= w * 0x9e3779b97f4a7c15ull;
        h = (h << 29 | h >> 35) * 0xbf58476d1ce4e5b9ull;
    };
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        mix(w);
    }
    if (i < n) {
        uint64_t w = 0;
        std::memcpy(&w, p + i, n - i);
        mix(w ^ (uint64_t{n - i} << 56));
    }
    return h;
}

bool aot_seal(const fs::path& dll) {
    HANDLE f = CreateFileW(dll.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    Seal seal{};
    std::memcpy(seal.magic, kSealMagic, sizeof seal.magic);
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0;
    if (ok) {
        seal.length = static_cast<uint64_t>(size.QuadPart);
        ok = hash_file(f, seal.length, &seal.hash);
    }
    LARGE_INTEGER end{};
    end.QuadPart = size.QuadPart;
    if (ok) ok = SetFilePointerEx(f, end, nullptr, FILE_BEGIN) && write_all(f, &seal, sizeof seal / 2);
    aot_fault_point("seal");  // half a seal: the check must refuse it
    if (ok) ok = write_all(f, reinterpret_cast<const uint8_t*>(&seal) + sizeof seal / 2, sizeof seal / 2);
    // On the disk before any name points at it: after a power loss NTFS
    // replays the rename from its journal, but not data still in the cache.
    if (ok) ok = FlushFileBuffers(f);
    CloseHandle(f);
    return ok;
}

AotFile aot_check_dll(const fs::path& dll, bool full) {
    // Shared for delete too: the cache may retire or replace it meanwhile.
    HANDLE f = CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? AotFile::kMissing : AotFile::kBroken;
    }
    AotFile result = AotFile::kUnsealed;
    LARGE_INTEGER size{};
    Seal seal{};
    DWORD got = 0;
    if (GetFileSizeEx(f, &size) && static_cast<uint64_t>(size.QuadPart) >= sizeof seal) {
        LARGE_INTEGER at{};
        at.QuadPart = size.QuadPart - static_cast<LONGLONG>(sizeof seal);
        if (SetFilePointerEx(f, at, nullptr, FILE_BEGIN) && ReadFile(f, &seal, sizeof seal, &got, nullptr) &&
            got == sizeof seal && std::memcmp(seal.magic, kSealMagic, sizeof seal.magic) == 0) {
            result = seal.length == static_cast<uint64_t>(at.QuadPart) ? AotFile::kSealed : AotFile::kBroken;
            uint64_t hash = 0;
            if (result == AotFile::kSealed && full && (!hash_file(f, seal.length, &hash) || hash != seal.hash)) {
                result = AotFile::kBroken;
            }
        }
    }
    CloseHandle(f);
    return result;
}

fs::path aot_temp_name(const fs::path& p) {
    static std::atomic<unsigned> counter{0};
    wchar_t tail[48];
    swprintf(tail, 48, L".%lx-%u.tmp", GetCurrentProcessId(), counter.fetch_add(1));
    return p.wstring() + tail;
}

bool aot_replace(const fs::path& from, const fs::path& to, int tries) {
    DWORD e = 0;
    for (int i = 0; i < tries; ++i) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        e = GetLastError();
        // Held open for a moment (a reader, a virus scanner): try again. A
        // DLL another process has loaded stays "access denied" every time.
        if (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED && e != ERROR_LOCK_VIOLATION) break;
        if (i + 1 < tries) Sleep(25u << i);
    }
    SetLastError(e);
    return false;
}

bool aot_write_file(const fs::path& p, const std::string& data) {
    const fs::path tmp = aot_temp_name(p);
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = write_all(f, data.data(), data.size() / 2);
    aot_fault_point("write");  // half written, under the temporary name
    ok = ok && write_all(f, data.data() + data.size() / 2, data.size() - data.size() / 2) && FlushFileBuffers(f);
    CloseHandle(f);
    if (ok && aot_replace(tmp, p)) return true;
    const DWORD e = GetLastError();
    DeleteFileW(tmp.c_str());
    SetLastError(e);
    return false;
}

bool aot_link_or_copy(const fs::path& from, const fs::path& to) {
    // Never into an old `to`: it may be another name of the cache's entry
    // (a take killed before its install), and a copy over it would write
    // into that entry under every app. When it cannot be deleted (an app has
    // that file loaded), it is renamed out of the way; failing that, nothing.
    if (!aot_retire(to)) return false;
    if (!aot_fault("nolink") && CreateHardLinkW(to.c_str(), from.c_str(), nullptr)) return true;
    return CopyFileW(from.c_str(), to.c_str(), TRUE) != 0;
}

bool aot_retire(const fs::path& p) {
    if (DeleteFileW(p.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) return true;
    const fs::path tmp = aot_temp_name(p);
    if (!MoveFileExW(p.c_str(), tmp.c_str(), 0)) return GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES;
    DeleteFileW(tmp.c_str());  // if another process holds it, a later pass does (aot_remove_stale_temps)
    return true;
}

std::string aot_read_file(const fs::path& p) {
    // Shared for delete: a writer's rename over it goes through while it is
    // read (a reader without it made aot_replace fail a third of the time
    // under a reader in a loop).
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::string s;
    if (f == INVALID_HANDLE_VALUE) return s;
    char buf[4096];
    for (DWORD n; ReadFile(f, buf, sizeof buf, &n, nullptr) && n > 0;) s.append(buf, n);
    CloseHandle(f);
    return s;
}

void aot_remove_stale_temps(const fs::path& dir, int64_t age_s) {
    std::error_code ec;
    const auto old = fs::file_time_type::clock::now() - std::chrono::seconds(age_s);
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::error_code e1;
        if (e.path().extension() != L".tmp" || !e.is_regular_file(e1)) continue;
        const auto t = e.last_write_time(e1);
        if (!e1 && t < old) fs::remove(e.path(), e1);
    }
}

void aot_touch(const fs::path& p) {
    HANDLE f = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    SetFileTime(f, nullptr, nullptr, &now);
    CloseHandle(f);
}

uint64_t aot_lib_id(const fs::path& so) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(so.c_str(), GetFileExInfoStandard, &a)) return 0;
    const uint64_t words[2] = {static_cast<uint64_t>(a.nFileSizeHigh) << 32 | a.nFileSizeLow,
                               static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32 |
                                   a.ftLastWriteTime.dwLowDateTime};
    const uint64_t id = aot_content_hash(words, sizeof words, 0x6c6962);
    return id != 0 ? id : 1;  // 0 means "no id" (a stamp from before ids)
}

bool aot_cache_take(const fs::path& cached, const fs::path& to) {
    std::error_code ec;
    if (!fs::exists(cached, ec)) return false;
    // Check what was linked, not the name: another worker may replace the
    // entry between a check and the link.
    if (!aot_link_or_copy(cached, to)) return false;
    if (aot_check_dll(to, true) != AotFile::kSealed) {
        DeleteFileW(to.c_str());
        const AotFile f = aot_check_dll(cached, true);
        if (f == AotFile::kBroken || f == AotFile::kUnsealed) {
            std::fprintf(stderr, "[win.apk] aot: %s in the shared cache is incomplete: deleted\n",
                         narrow(cached).c_str());
            aot_retire(cached);
        }
        return false;
    }
    aot_touch(to);  // a link: the cache's entry too
    aot_touch(cached);
    return true;
}

bool aot_cache_put(const fs::path& dll, const fs::path& cached) {
    // The first good entry stays: an app may have it loaded, and a mapped DLL
    // can be neither replaced nor deleted.
    if (aot_check_dll(cached, true) == AotFile::kSealed) {
        aot_touch(cached);
        return true;
    }
    const fs::path tmp = aot_temp_name(cached);
    bool ok = aot_link_or_copy(dll, tmp);
    aot_fault_point("cache-put");
    ok = ok && aot_replace(tmp, cached);
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

void aot_cache_trim(const fs::path& cache, uintmax_t max_bytes, int max_days) {
    std::error_code ec;
    aot_remove_stale_temps(cache);
    const auto now = fs::file_time_type::clock::now();
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    uintmax_t total = 0;
    for (const auto& e : fs::directory_iterator(cache, ec)) {
        std::error_code e1;
        if (!e.is_regular_file(e1) || e.path().extension() == L".tmp") continue;
        auto t = e.last_write_time(e1);
        if (e1) continue;
        // Used "in the future": the clock was ahead then. As now, or the
        // entry would outlive everything else.
        if (t > now + std::chrono::hours(1)) {
            aot_touch(e.path());
            t = now;
        }
        total += e.file_size(e1);
        files.emplace_back(t, e.path());
    }
    std::sort(files.begin(), files.end());
    const auto stale = now - std::chrono::hours(24 * max_days);
    for (const auto& f : files) {
        if (total <= max_bytes && f.first >= stale) break;
        const uintmax_t size = fs::file_size(f.second, ec);
        if (fs::remove(f.second, ec)) total -= size;  // not one an app has loaded
    }
}

bool aot_fault(const char* name) {
    const std::string& spec = fault_spec();
    return !spec.empty() && spec.find("," + std::string(name) + ",") != std::string::npos;
}

void aot_fault_point(const char* step) {
    if (fault_spec().empty() || !aot_fault(("kill:" + std::string(step)).c_str())) return;
    std::fflush(stdout);
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 99);
}

}  // namespace winapk::arm64
