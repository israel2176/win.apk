// crash_report.h -- what the log and the logs folder say when the process dies.
//
// A crash in an app's native code, or in the host, ended the run with an exit
// code and nothing else: the compatibility sweep showed 0xC0000005 for Pixel
// Wheels, LocalSend, Jitsi Meet and Alex vs Bus and 0xC0000409 (a fail-fast
// abort) for VLC, with no line saying where. install_crash_report adds a last
// vectored exception handler that names the faulting instruction (the guest
// library and symbol, or the host module) and the guest or host frames it can
// find on the stack, a SIGABRT handler for the host CRT's abort, and a
// terminate handler for an uncaught C++ exception, and an unhandled-exception
// filter for the fault nobody handled, whatever module it is in (the vectored
// handler leaves the JVM's and Windows' modules alone, and Pixel Wheels died
// in one with nothing said). All of them only report; whoever handled the
// fault before (the guest's own signal handlers, which the Bionic shim
// delivers first, the arm64 bridge, the AOT fault reporter) keeps doing so,
// and HotSpot's structured exception handling still gets its own faults
// afterwards.
//
// With set_crash_dump_location, the report also writes a minidump,
// logs\crash-<app>-<yyyymmdd-hhmmss>.dmp (MiniDumpWriteDump, with the memory
// the stacks refer to and the thread list), keeps the last three per app and
// logs the file's name next to the faulting module and offset, so a user can
// attach it to an issue and a developer can open it in WinDbg or Visual
// Studio.

#pragma once

#include <filesystem>
#include <string>

namespace winapk::host {

void install_crash_report();

// After JNI_CreateJavaVM: HotSpot sets its own unhandled-exception filter, so
// put the reporter back in front of it (it chains to HotSpot's).
void rearm_crash_report();

// Where crash dumps go and what they are named after (the app's library
// name). Loads dbghelp now, not in the handler.
void set_crash_dump_location(const std::filesystem::path& dir, const std::wstring& app);

}  // namespace winapk::host
