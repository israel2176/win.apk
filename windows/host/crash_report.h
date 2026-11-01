// crash_report.h -- what the log says when the process dies.
//
// A crash in an app's native code, or in the host, ended the run with an exit
// code and nothing else: the compatibility sweep showed 0xC0000005 for Pixel
// Wheels, LocalSend, Jitsi Meet and Alex vs Bus and 0xC0000409 (a fail-fast
// abort) for VLC, with no line saying where. install_crash_report adds a last
// vectored exception handler that names the faulting instruction (the guest
// library and symbol, or the host module) and the guest or host frames it can
// find on the stack, a SIGABRT handler for the host CRT's abort, and a
// terminate handler for an uncaught C++ exception. All three only report;
// whoever handled the fault before (HotSpot's own signal handling, the arm64
// bridge, the AOT fault reporter) keeps doing so.

#pragma once

namespace winapk::host {

void install_crash_report();

}  // namespace winapk::host
