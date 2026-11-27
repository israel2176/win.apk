// long_path.h -- a host path past MAX_PATH as Windows' file calls take it.
//
// An app's files sit deep: its libraries under android\data\app\~~<id>\<package>-<id>\lib\<abi>,
// its data under android\data\data\<package>, both below %LOCALAPPDATA%\Win.apk. With a long
// user name or package such a path passes 260 characters, and where Windows' long-path policy
// is off (most PCs) every plain call on it fails as "not found". The "\\?\" form of the full
// path has no such limit. Everything that names a host file to Windows on a path it did not
// make itself goes through long_path; what is shown or handed back (dladdr, error messages,
// realpath) goes through plain_path.

#pragma once

#include <string>

#include <windows.h>

namespace winapk {

// A path near MAX_PATH by the long-path form of its full path (made full first: under "\\?\"
// Windows takes neither '/' nor ".." nor a relative path); a shorter one, or one already in
// that form, as it is.
inline std::wstring long_path(const std::wstring& w) {
    if (w.size() < MAX_PATH - 12 || w.rfind(L"\\\\?\\", 0) == 0) return w;
    const DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
    if (n == 0) return w;
    std::wstring full(n, L'\0');
    const DWORD m = GetFullPathNameW(w.c_str(), n, &full[0], nullptr);
    if (m == 0 || m >= n) return w;
    full.resize(m);
    if (full.rfind(L"\\\\?\\", 0) == 0) return full;
    if (full.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + full.substr(2);
    return L"\\\\?\\" + full;
}

// The usual spelling of a long-path form (what GetFinalPathNameByHandleW gives, or long_path):
// "\\?\C:\x" is "C:\x", "\\?\UNC\server\share\x" is "\\server\share\x"; anything else as it is.
inline std::wstring plain_path(std::wstring w) {
    if (w.compare(0, 8, L"\\\\?\\UNC\\") == 0) w.replace(0, 8, L"\\\\");
    else if (w.compare(0, 4, L"\\\\?\\") == 0) w.erase(0, 4);
    return w;
}

}  // namespace winapk
