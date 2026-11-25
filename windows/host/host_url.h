// host_url.h -- which URLs the runtime hands to Windows to open (NativeCocoa.openHostUrl, ShellExecute). Pure (no
// Windows call, no JNI), so test/host_url_test.cpp checks it on its own.
//
// On a device an implicit ACTION_VIEW reaches only the apps that declare its scheme, and one nobody declares is an
// ActivityNotFoundException. Here the "apps" are Windows' own protocol handlers, and ShellExecute starts whatever
// handler a scheme is registered to: search-ms: (a remote share's files shown as local ones), ms-msdt:, ms-officecmd:
// and the like have carried code execution from a single link. So only the schemes the Java side has a host
// action for go out (com.macapk.loader.ActivityHost.hostActionForDirect: http, https, geo, mailto, tel; mailto: for a
// text share; and the Windows settings page an app's "turn on location" opens, ms-settings:), the scheme written as
// RFC 3986 says (a letter, then letters, digits, "+", "-", "."), compared without case. file: and a drive path are
// never among them.
#pragma once

#include <cwctype>
#include <string>

namespace winapk::host {

inline bool may_open_on_host(const std::wstring& url) {
    const size_t colon = url.find(L':');
    if (colon == std::wstring::npos || colon == 0 || !std::iswalpha(url[0])) return false;
    std::wstring scheme;
    for (size_t i = 0; i < colon; ++i) {
        const wchar_t c = url[i];
        if (c > 0x7f || !(std::iswalnum(c) || c == L'+' || c == L'-' || c == L'.')) return false;
        scheme += static_cast<wchar_t>(std::towlower(c));
    }
    for (const wchar_t c : url) {
        if (c < L' ' || c == 0x7f) return false;  // a control character: no handler is given one
    }
    if (scheme == L"http" || scheme == L"https") return url.compare(colon, 3, L"://") == 0;
    return scheme == L"mailto" || scheme == L"tel" || scheme == L"geo" || scheme == L"ms-settings";
}

}  // namespace winapk::host
