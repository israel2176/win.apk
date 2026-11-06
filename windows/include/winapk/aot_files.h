// aot_files.h -- the files of the arm64 translation, written so that a crash,
// a kill or a second process at any moment leaves nothing a reader could
// take for a translation it is not.
//
// Three kinds of process touch them at once: an app's loader (arm64_aot.cpp)
// reads <lib>.so.aot.dll and the stamp and writes the stamp ("bad",
// "written"); that app's background worker (runtime_main.cpp,
// --aot-translate) writes the DLL, the stamp and its work folder; and the
// workers of every other app read and write the shared cache
// (<data>\aot-cache), whose entries are hard links to the apps' DLLs. Before
// these rules, a stamp could say "full" for a DLL that was cut short (a copy
// killed half way, a power loss before the data reached the disk), and
// nothing ever translated that library again; two workers wrote the cache
// through the same temporary name; and a failed write left its temporary file
// behind for good.
//
// The rules:
//   - Nothing is written in place. A file is written under a name of its own
//     (aot_temp_name: the process and a counter, so two writers never share
//     one), flushed to the disk, then renamed over the old one (aot_replace,
//     which retries while another process holds the old one open for a
//     moment, as a virus scanner does). A reader sees the old file or the
//     new one, whole.
//   - A finished DLL is sealed (aot_seal): 32 bytes appended after the
//     linker's output, which Windows' loader ignores as it ignores a
//     signature, saying how long the output is and its hash. A file cut
//     short, or zeros where data never reached the disk, fails the check
//     (aot_check_dll) and is never loaded, installed or shared. A stamp
//     written by this code says its DLLs are sealed (the library id at its
//     end); a DLL from before seals is loaded only under an older stamp.
//   - The shared cache is checked whole before an entry is used, and an entry
//     that fails is moved aside and deleted. An entry is never replaced while
//     it is good: an app may have it mapped, and Windows refuses to replace
//     (or delete) a mapped DLL, so the first writer wins and the second drops
//     its copy.
//   - A temporary file left by a process that was killed is deleted by the
//     next pass over its folder, once it is an hour old (no writer takes that
//     long between creating and renaming one).
//
// WINAPK_AOT_FAULT=<point>[,<point>...] (tests only): "nolink" makes every
// link a copy (a file system without hard links); "kill:<step>" ends the
// process at once at that step (aot_fault_point), as a crash or Task Manager
// would: "seal" (half a seal written), "cache-put" (an entry under its
// temporary name), "write" (half of an aot_write_file), and in the worker
// (runtime_main.cpp) "install" and "installed" (before and after the rename
// into place).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace winapk::arm64 {

// What aot_check_dll found.
enum class AotFile {
    kSealed,    // complete: as long as its seal says, and (when hashed) the same bytes
    kUnsealed,  // no seal: a DLL from before seals, or one cut short before its seal
    kBroken,    // a seal that does not match: cut short, overwritten, or zeros
    kMissing,
};

// The size of the seal at the end of a sealed DLL.
constexpr size_t kAotSealBytes = 32;

// Hash of a file's contents, as the seal records it (64 bits a step: a 200 MB
// DLL takes a few tens of milliseconds).
uint64_t aot_content_hash(const void* data, size_t n, uint64_t seed);

// Appends the seal to `dll` and flushes it to the disk. Only for a file this
// process just made (never a name that may be a hard link to another).
bool aot_seal(const std::filesystem::path& dll);

// Whether `dll` is complete. `full` also hashes its contents; without it the
// check reads only the seal and the file's size (cheap enough for every
// start of an app).
AotFile aot_check_dll(const std::filesystem::path& dll, bool full);

// `p` + ".<pid>-<n>.tmp": unique to this process and call.
std::filesystem::path aot_temp_name(const std::filesystem::path& p);

// Renames `from` over `to`, retrying for about a second while either is in
// use; false (GetLastError says why) when it cannot, as when `to` is a DLL
// another process has loaded.
bool aot_replace(const std::filesystem::path& from, const std::filesystem::path& to, int tries = 6);

// Writes `data` to `p` atomically (temporary name, flush, rename).
bool aot_write_file(const std::filesystem::path& p, const std::string& data);

// `to` as a second name of `from` (a hard link), or a copy where the file
// system has no hard links. An old `to` is retired first, never written into.
bool aot_link_or_copy(const std::filesystem::path& from, const std::filesystem::path& to);

// Deletes a file, or when it cannot be (another process has it loaded),
// renames it out of the way, to be deleted by a later pass
// (aot_remove_stale_temps). True when the name is free.
bool aot_retire(const std::filesystem::path& p);

// A small file's contents ("" when it cannot be read), opened so that a
// writer can rename over it meanwhile.
std::string aot_read_file(const std::filesystem::path& p);

// Deletes the files named *.tmp in `dir` that are more than `age_s` seconds
// old: what writers that were killed left.
void aot_remove_stale_temps(const std::filesystem::path& dir, int64_t age_s = 3600);

// Sets a file's last write time to now (the shared cache's "last used").
void aot_touch(const std::filesystem::path& p);

// A library's identity for its stamp: its size and last write time, hashed.
// Unlike a comparison of the stamp's time with the library's, it does not
// depend on the clock: a clock set back made every translation look older
// than its library, and the worker translated it again at every pass.
uint64_t aot_lib_id(const std::filesystem::path& so);

// ---- the shared cache ----
// `cached` is <cache>\<key>.aot.dll.

// Takes the cache's translation into `to` (a new name in the app's work
// folder); false when there is none or it is broken (then it is retired).
bool aot_cache_take(const std::filesystem::path& cached, const std::filesystem::path& to);

// Puts the sealed `dll` into the cache, unless a good entry is already there.
bool aot_cache_put(const std::filesystem::path& dll, const std::filesystem::path& cached);

// Keeps the cache within `max_bytes`, least recently used first, drops what
// was not used for `max_days` (a new Win.apk gives every library a new key,
// and the old entries would otherwise stay until the cache is full), and
// deletes stale temporary files. An entry an app has loaded cannot be deleted
// and stays (the app keeps its own name for it in any case). An entry used
// "in the future" (the clock was ahead) ages from now.
void aot_cache_trim(const std::filesystem::path& cache, uintmax_t max_bytes, int max_days);

// ---- fault injection (WINAPK_AOT_FAULT) ----
bool aot_fault(const char* name);
// Ends the process at once when WINAPK_AOT_FAULT has "kill:<step>".
void aot_fault_point(const char* step);

}  // namespace winapk::arm64
