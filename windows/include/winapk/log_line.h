// log_line.h -- one log line to stderr in one write.
//
// A headless run's stderr is unbuffered (host/runtime_main.cpp, buffer_stderr), and the C runtime writes
// an unbuffered stream a character at a time. A line another thread wrote meanwhile to the same file
// through stdout -- the layout sweep's and the UI probe's "PROBE ..." lines, which the checks grep for at
// the start of a line -- then landed inside the log line ("[I] Win.apk.SurfaceView: PROBE ...") and
// was lost to the check. The logs threads write while those lines go out (the framework's NativeLog,
// a guest library's liblog) go through here: the line whole, in one WriteFile while the stream holds
// nothing buffered, else into the stream's buffer as one piece.
#pragma once

#include <cstdio>
#include <string>

#include <windows.h>

namespace winapk {

inline void write_stderr_line(const std::string& line) {
    static const bool unbuffered = [] {
        char v[8];
        return GetEnvironmentVariableA("WINAPK_HEADLESS", v, sizeof v) > 0;
    }();
    HANDLE h = unbuffered ? GetStdHandle(STD_ERROR_HANDLE) : nullptr;
    DWORD written = 0;
    if (h && h != INVALID_HANDLE_VALUE &&
        WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr))
        return;
    std::fwrite(line.data(), 1, line.size(), stderr);
}

}  // namespace winapk
