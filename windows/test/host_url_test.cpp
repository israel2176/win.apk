// host_url_test.cpp -- winapk-host-url-test: which URLs the runtime hands to Windows to open (host/host_url.h).
// No window and no Windows call.
//
//   winapk-host-url-test

#include <cstdio>
#include <string>

#include "host_url.h"

namespace {

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

using winapk::host::may_open_on_host;

}  // namespace

int main() {
    std::printf("the schemes with a host action\n");
    check("https", may_open_on_host(L"https://example.com/a?b=c#d"));
    check("http, any case", may_open_on_host(L"HTTP://example.com/"));
    check("mailto with a subject and body", may_open_on_host(L"mailto:a@example.com?subject=Hi&body=x%20y"));
    check("tel", may_open_on_host(L"tel:+972-3-555-0000"));
    check("geo", may_open_on_host(L"geo:32.08,34.78?q=Tel+Aviv"));
    check("the location settings page", may_open_on_host(L"ms-settings:privacy-location"));

    std::printf("Windows' other protocol handlers\n");
    check("search-ms", !may_open_on_host(L"search-ms:query=x&crumb=location:\\\\203.0.113.5\\share"));
    check("ms-msdt", !may_open_on_host(L"ms-msdt:/id PCWDiagnostic /skip force"));
    check("ms-officecmd", !may_open_on_host(L"ms-officecmd:{\"id\":1}"));
    check("shell", !may_open_on_host(L"shell:startup"));
    check("an app's own scheme", !may_open_on_host(L"myapp://callback?code=1"));
    check("javascript", !may_open_on_host(L"javascript:alert(1)"));

    std::printf("files and paths\n");
    check("file:", !may_open_on_host(L"file:///C:/Windows/System32/calc.exe"));
    check("FILE: in capitals", !may_open_on_host(L"FILE://server/share/x.exe"));
    check("a drive path", !may_open_on_host(L"C:\\Windows\\System32\\calc.exe"));
    check("a share", !may_open_on_host(L"\\\\server\\share\\x.exe"));
    check("a relative name", !may_open_on_host(L"calc.exe"));
    check("http without //", !may_open_on_host(L"http:calc.exe"));

    std::printf("malformed\n");
    check("empty", !may_open_on_host(L""));
    check("no scheme", !may_open_on_host(L":x"));
    check("a scheme starting with a digit", !may_open_on_host(L"1http://x"));
    check("a space in the scheme", !may_open_on_host(L"ht tp://x"));
    check("a non-ASCII letter in the scheme", !may_open_on_host(L"htt\x0440://x"));
    check("a control character", !may_open_on_host(L"https://example.com/\nx"));

    if (g_failures) {
        std::printf("%d FAILED\n", g_failures);
        return 1;
    }
    std::printf("ALL PASSED\n");
    return 0;
}
