// webview_file_urls_test.cpp -- winapk-webview-urls-test: the app's file: URLs
// on WebView2 (host/webview_file_urls.h): the mapping both ways, a data page's
// references, a base's request form, and which pages may read the file origin.
// No window and no Windows call.
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

}  // namespace

int main() {
    std::printf("the mapping\n");
    // Yahav's Cordova page and its script.
    const std::wstring page = L"file:///android_asset/www/app/index.html?cordova";
    const std::wstring loaded = engine_url(page);
    check("an asset page loads on the file origin, query kept",
          loaded == L"https://winapk-file.example/android_asset/www/app/index.html?cordova");
    check("and the app sees its own URL again", guest_url(loaded) == page);
    check("../js/cordova.js resolves on the origin, back as the asset",
          guest_url(L"https://winapk-file.example/android_asset/www/js/cordova.js") ==
              L"file:///android_asset/www/js/cordova.js");
    check("android_res", engine_url(L"file:///android_res/raw/help.html") ==
                             L"https://winapk-file.example/android_res/raw/help.html");
    check("FILE:/// in any case", engine_url(L"FILE:///android_asset/a.html") ==
                                      L"https://winapk-file.example/android_asset/a.html");
    check("a share (file://host/x) is left alone", engine_url(L"file://server/x.html") == L"file://server/x.html");
    check("a web URL is left alone", engine_url(L"https://example.com/") == L"https://example.com/");
    check("the origin alone is file:///", guest_url(L"https://winapk-file.example") == L"file:///");
    check("the host without case", guest_url(L"https://WINAPK-FILE.example/a") == L"file:///a");
    check("another host is not the origin", !on_file_origin(L"https://winapk-file.example.evil.net/a"));
    check("nor a longer name", !on_file_origin(L"https://winapk-file.examplex/a"));
    check("nor http", !on_file_origin(L"http://winapk-file.example/a"));

    std::printf("a data page's references\n");
    check("src=\"file:///android_asset/...\"",
          rewrite_file_refs(L"<script src=\"file:///android_asset/a.js\"></script>") ==
              L"<script src=\"https://winapk-file.example/android_asset/a.js\"></script>");
    check("url('file:///...') and url(file:///...)",
          rewrite_file_refs(L"a{background:url('file:///android_res/drawable/x.png')} b{x:url(file:///y)}") ==
              L"a{background:url('https://winapk-file.example/android_res/drawable/x.png')} "
              L"b{x:url(https://winapk-file.example/y)}");
    check("an unquoted attribute", rewrite_file_refs(L"<img src=file:///a.png>") ==
                                       L"<img src=https://winapk-file.example/a.png>");
    check("the page's text keeps the file:/// it shows",
          rewrite_file_refs(L"<p>see file:///android_asset/</p>") == L"<p>see file:///android_asset/</p>");
    check("nothing to change", rewrite_file_refs(L"<p>hello</p>") == L"<p>hello</p>");

    std::printf("a base's request\n");
    check("no fragment", request_form(L"https://example.com/a.html#top") == L"https://example.com/a.html");
    check("an empty path is /", request_form(L"https://example.com") == L"https://example.com/");
    check("before a query too", request_form(L"https://example.com?x=1") == L"https://example.com/?x=1");
    check("the file origin's base", request_form(engine_url(L"file:///android_asset/")) ==
                                        L"https://winapk-file.example/android_asset/");
    check("origin_of", origin_of(L"https://Example.com:8443/a?b") == L"https://example.com:8443");
    check("no origin for null", origin_of(L"null").empty());

    std::printf("who may read the file origin\n");
    check("a file: page", file_request_allowed(true, L"", L"", L"https://winapk-file.example/android_asset/i.html"));
    check("a CORS request of a file: page",
          file_request_allowed(true, L"", L"https://winapk-file.example", L""));
    check("the app's about:blank data page (Origin: null)", file_request_allowed(true, L"", L"null", L""));
    check("a data page with a web base, from that base",
          file_request_allowed(true, L"https://example.com", L"", L"https://example.com/"));
    check("a web page is refused", !file_request_allowed(false, L"", L"", L"https://evil.example/"));
    check("a web page with no Referer is refused too", !file_request_allowed(false, L"", L"", L""));
    check("a web frame inside a file: page is refused",
          !file_request_allowed(true, L"", L"", L"https://evil.example/frame.html"));
    check("a web origin's CORS request is refused",
          !file_request_allowed(true, L"", L"https://evil.example", L""));
    check("another web origin than the data page's base is refused",
          !file_request_allowed(true, L"https://example.com", L"", L"https://evil.example/"));

    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
