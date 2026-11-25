// webview_file_urls_test.cpp -- winapk-webview-urls-test: the app's file: URLs on WebView2
// (host/webview_file_urls.h): the mapping both ways, a data page's references, a base's request form, origins as a
// browser writes them, which pages may read the file origin, and the byte ranges media asks for. No window and no
// Windows call.
//
//   winapk-webview-urls-test

#include <cstdio>
#include <string>

#include "webview_file_urls.h"

namespace {

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

using namespace winapk::host::wvfile;

bool part(const ByteRange& r, uint64_t first, uint64_t last) {
    return r.kind == ByteRange::kPart && r.first == first && r.last == last;
}

}  // namespace

int main() {
    std::printf("the mapping\n");
    // Yahav's Cordova page and its script.
    const std::wstring page = L"file:///android_asset/www/app/index.html?cordova";
    const std::wstring loaded = engine_url(page);
    check("an asset page loads on the file origin, its query kept",
          loaded == L"https://winapk-file.example/android_asset/www/app/index.html?cordova");
    check("and the app sees its own URL again", guest_url(loaded) == page);
    check("a script beside it comes back as the asset",
          guest_url(L"https://winapk-file.example/android_asset/www/js/cordova.js") ==
              L"file:///android_asset/www/js/cordova.js");
    check("android_res", engine_url(L"file:///android_res/raw/help.html") ==
                             L"https://winapk-file.example/android_res/raw/help.html");
    check("the scheme in any case", engine_url(L"FILE:///android_asset/a.html") ==
                                        L"https://winapk-file.example/android_asset/a.html");
    check("file://localhost/ is the same file (RFC 8089)",
          engine_url(L"file://localhost/android_asset/a.html") ==
              L"https://winapk-file.example/android_asset/a.html");
    check("file:/x, one slash, too", engine_url(L"file:/android_asset/a.html") ==
                                         L"https://winapk-file.example/android_asset/a.html");
    check("a fragment is kept", engine_url(L"file:///android_asset/a.html#top") ==
                                    L"https://winapk-file.example/android_asset/a.html#top");
    check("a share (file://host/x) is left to WebView2",
          engine_url(L"file://server/x.html") == L"file://server/x.html");
    check("a web URL is left alone", engine_url(L"https://example.com/") == L"https://example.com/");
    check("the origin alone is file:///", guest_url(L"https://winapk-file.example") == L"file:///");
    check("the origin with a query alone", guest_url(L"https://winapk-file.example?x") == L"file:///?x");
    check("the host in any case", guest_url(L"https://WINAPK-FILE.example/a") == L"file:///a");
    check("its default port spelled out", guest_url(L"https://winapk-file.example:443/a") == L"file:///a");
    check("a web URL comes back as it is", guest_url(L"https://example.com/a") == L"https://example.com/a");
    check("a longer host is not the origin", !on_file_origin(L"https://winapk-file.example.evil.net/a"));
    check("nor a longer name", !on_file_origin(L"https://winapk-file.examplex/a"));
    check("nor another port", !on_file_origin(L"https://winapk-file.example:8443/a"));
    check("nor http", !on_file_origin(L"http://winapk-file.example/a"));

    std::printf("a data page's references\n");
    check("src=\"file:///android_asset/...\"",
          rewrite_file_refs(L"<script src=\"file:///android_asset/a.js\"></script>") ==
              L"<script src=\"https://winapk-file.example/android_asset/a.js\"></script>");
    check("url('file:///...') and url(file:///...)",
          rewrite_file_refs(L"a{background:url('file:///android_res/drawable/x.png')} b{x:url(file:///y)}") ==
              L"a{background:url('https://winapk-file.example/android_res/drawable/x.png')} "
              L"b{x:url(https://winapk-file.example/y)}");
    check("url( with a space before it", rewrite_file_refs(L"b{x:url( file:///y)}") ==
                                             L"b{x:url( https://winapk-file.example/y)}");
    check("an unquoted attribute, and one with spaces around =",
          rewrite_file_refs(L"<img src=file:///a.png><img src = file:///b.png>") ==
              L"<img src=https://winapk-file.example/a.png><img src = https://winapk-file.example/b.png>");
    check("a script's string", rewrite_file_refs(L"x = 'file:///android_asset/d.json';") ==
                                   L"x = 'https://winapk-file.example/android_asset/d.json';");
    check("the page's text keeps the file:/// it shows",
          rewrite_file_refs(L"<p>see file:///android_asset/</p>") == L"<p>see file:///android_asset/</p>");
    check("nor a ( that is not url(", rewrite_file_refs(L"<p>(file:///x)</p>") == L"<p>(file:///x)</p>");
    check("nothing to change", rewrite_file_refs(L"<p>hello</p>") == L"<p>hello</p>");
    check("at the very start", rewrite_file_refs(L"file:///x") == L"file:///x");

    std::printf("a base's request\n");
    check("no fragment", request_form(L"https://example.com/a.html#top") == L"https://example.com/a.html");
    check("an empty path is /", request_form(L"https://example.com") == L"https://example.com/");
    check("before a query too", request_form(L"https://example.com?x=1") == L"https://example.com/?x=1");
    check("the file origin's base", request_form(engine_url(L"file:///android_asset/")) ==
                                        L"https://winapk-file.example/android_asset/");

    std::printf("origins\n");
    check("scheme and host in lower case, a port kept",
          origin_of(L"https://Example.com:8443/a?b") == L"https://example.com:8443");
    check("a default port dropped", origin_of(L"https://example.com:443/") == L"https://example.com" &&
                                        origin_of(L"http://example.com:80") == L"http://example.com");
    check("not another scheme's default", origin_of(L"http://example.com:443/") == L"http://example.com:443");
    check("user info dropped", origin_of(L"https://user:pw@example.com/") == L"https://example.com");
    check("an IPv6 host keeps its colons", origin_of(L"http://[::1]:8080/") == L"http://[::1]:8080" &&
                                               origin_of(L"http://[::1]/") == L"http://[::1]");
    check("no origin for null, about:, data:, file:",
          origin_of(L"null").empty() && origin_of(L"about:blank").empty() && origin_of(L"data:text/html,x").empty() &&
              origin_of(L"file:///a").empty());

    std::printf("who may read the file origin\n");
    check("a file: page", file_request_allowed(true, L"", L"", L"https://winapk-file.example/android_asset/i.html"));
    check("a CORS request of a file: page", file_request_allowed(true, L"", L"https://winapk-file.example", L""));
    check("a top-level load with neither header", file_request_allowed(true, L"", L"", L""));
    check("the app's about:blank data page (Origin: null)", file_request_allowed(true, L"", L"null", L""));
    check("a data page with a web base, from that base",
          file_request_allowed(true, L"https://example.com", L"", L"https://example.com/") &&
              file_request_allowed(true, L"https://example.com", L"https://example.com", L"https://example.com/x"));
    check("a web page is refused", !file_request_allowed(false, L"", L"", L"https://evil.example/"));
    check("a web page with no Referer is refused too", !file_request_allowed(false, L"", L"", L""));
    check("a web frame inside a file: page is refused",
          !file_request_allowed(true, L"", L"", L"https://evil.example/frame.html"));
    check("a web origin's CORS request is refused", !file_request_allowed(true, L"", L"https://evil.example", L""));
    check("a web origin's request with the file origin's Referer is refused all the same",
          !file_request_allowed(true, L"", L"https://evil.example", L"https://winapk-file.example/a.html"));
    check("another web origin than the data page's base is refused",
          !file_request_allowed(true, L"https://example.com", L"", L"https://evil.example/"));
    check("a look-alike of the file origin is refused",
          !file_request_allowed(true, L"", L"", L"https://winapk-file.example.evil.net/"));

    std::printf("byte ranges\n");
    check("no Range: the whole body", byte_range(L"", 1000).kind == ByteRange::kWhole);
    check("bytes=0-499", part(byte_range(L"bytes=0-499", 1000), 0, 499));
    check("an open end runs to the last byte", part(byte_range(L"bytes=500-", 1000), 500, 999));
    check("an end past the body is cut to it", part(byte_range(L"bytes=900-5000", 1000), 900, 999));
    check("the last 100 bytes", part(byte_range(L"bytes=-100", 1000), 900, 999));
    check("a suffix longer than the body is all of it", part(byte_range(L"bytes=-5000", 1000), 0, 999));
    check("the unit in any case, spaces around", part(byte_range(L"Bytes = 1-2 ", 1000), 1, 2));
    check("one byte", part(byte_range(L"bytes=999-999", 1000), 999, 999));
    check("a start past the body: 416", byte_range(L"bytes=1000-", 1000).kind == ByteRange::kUnsatisfiable);
    check("a zero-length suffix: 416", byte_range(L"bytes=-0", 1000).kind == ByteRange::kUnsatisfiable);
    check("any range of an empty body: 416", byte_range(L"bytes=0-", 0).kind == ByteRange::kUnsatisfiable);
    check("last before first: ignored, the whole body", byte_range(L"bytes=5-1", 1000).kind == ByteRange::kWhole);
    check("several ranges: the whole body", byte_range(L"bytes=0-1,5-6", 1000).kind == ByteRange::kWhole);
    check("another unit: the whole body", byte_range(L"items=0-1", 1000).kind == ByteRange::kWhole);
    check("junk: the whole body", byte_range(L"bytes=a-b", 1000).kind == ByteRange::kWhole &&
                                      byte_range(L"bytes=", 1000).kind == ByteRange::kWhole &&
                                      byte_range(L"bytes=-", 1000).kind == ByteRange::kWhole &&
                                      byte_range(L"bytes 0-1", 1000).kind == ByteRange::kWhole);
    check("a number too large for 64 bits: the whole body",
          byte_range(L"bytes=99999999999999999999999-", 1000).kind == ByteRange::kWhole);

    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
