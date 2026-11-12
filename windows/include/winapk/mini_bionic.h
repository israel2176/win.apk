// mini_bionic.h -- the Windows Bionic shim: its core and the name lookup.
//
// The macOS shim (runtime/translator/src/bionic_shim.cpp) exports Bionic's
// names straight out of a dylib, because there an Android arm64 caller and a
// macOS callee agree on the calling convention. On Windows x64 they do not,
// so every export here is a WINAPK_GUEST_ABI wrapper around the Windows CRT,
// handed to the ELF loader through a name table instead of the PE export table.
//
// mini_bionic.cpp holds the core (allocation, errno, the C++ atexit pair,
// qsort's guest comparator) and the lookup; the rest lives in src/bionic/,
// one module per area, each with its own table (bionic_internal.h).

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace winapk {

// Host address of a shim export, or nullptr if the shim does not have it.
void* mini_bionic_resolve(const char* name);

// Runs the destructors guest code registered with __cxa_atexit, newest first.
void mini_bionic_run_atexit();
// The same for the entries whose DSO handle lies in [base, base + size): what
// dlclose owes a library before it is unmapped (its crtbegin's __on_dlclose
// runs them from DT_FINI_ARRAY; this catches a library without one, and
// keeps no pointer into unmapped memory for process exit).
void mini_bionic_run_atexit_in(const void* base, size_t size);

// Installs the vectored exception handler that delivers faults to guest
// signal handlers (src/bionic/signal.cpp). Idempotent; the guest linker's
// constructor calls it, and the runtime's crash report calls it first so the
// guest's handlers run before the report.
void guest_signals_install();
// The calling thread's alternate signal stack, as Bionic gives every thread
// one (60 KiB, committed when a handler first runs on it); once per thread.
// Called where a thread first runs guest code.
void guest_signal_altstack_for_current_thread();

// fts_open for a caller whose struct stat is not x86_64's (an arm64 library):
// `fill` writes each entry's fts_statp from the shim's x86_64 stat. `compar`
// is the caller's comparator, callable as a System V function.
using FtsStatFill = void (*)(const void* x86_64_stat, void* out);
void* fts_open_with_stat(char* const* argv, int options, void* compar, FtsStatFill fill);

// The guest's system properties (__system_property_get). The Java framework
// pushes its values here (NativeCocoa.setSystemProperty); null erases.
void property_set(const char* name, const char* value);

// The guest's environment (getenv/setenv), apart from the host's. setenv and
// unsetenv return 0 or -errno (Linux numbering), as NativeOs expects.
const char* bionic_getenv(const char* name);
int bionic_setenv(const char* name, const char* value, int overwrite);
int bionic_unsetenv(const char* name);

// Where Android's absolute paths live (src/bionic/paths.cpp): "/x" is
// <android root>/x, and /system/fonts and /system/etc/fonts.xml are the fonts
// directory Boot writes. UTF-8 host paths.
void set_android_root(const char* dir);
void set_system_fonts_dir(const char* dir);
// The running app: /data/user/0/<package> and /data/data/<package> are
// `host_dir`, its data directory (UTF-8).
void set_app_data(const char* package, const char* host_dir);
// The host file an Android path names (host_path), for the runtime's own
// natives (java.io, java.nio); a host path comes back as it is.
std::wstring android_host_path(const char* path);
// The Android name of a file as a listing of its host directory names it: an
// Android name's characters that Windows forbids (< > : " | ? *, control
// characters, a final dot or space) are stored as U+F000 + the character
// (paths.cpp), and this turns them back.
std::wstring android_file_name(std::wstring host_name);

// The host file behind a /proc path that is a real file (/proc/meminfo,
// /proc/self/cmdline and /proc/<pid>/cmdline, which Boot writes under
// <android root>/proc), for java.io.FileReader's /proc hook; empty when there
// is none.
std::wstring android_proc_file(const char* path);

// The calling thread's NDK looper (ALooper, src/bionic/looper.cpp), for Java's
// android.os.Looper (NativeLooper): one wait for a thread's Java messages and
// its native descriptors. poll_once returns ALooper's POLL_* codes.
void* looper_for_current_thread();
int looper_poll_once(void* looper, int timeout_ms);
void looper_wake(void* looper);
bool looper_has_fds(void* looper);

// The threads the guest started with pthread_create and that still run, for the debug agent
// (host/debug_agent.cpp): each one's Windows thread id and the guest function it started in.
// `out` takes up to `max` of them; returns how many there are.
struct GuestThread {
    uint32_t tid;
    const void* start;
};
size_t guest_threads(GuestThread* out, size_t max);

}  // namespace winapk
