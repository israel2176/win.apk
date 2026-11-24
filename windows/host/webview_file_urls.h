// webview_file_urls.h -- the app's file: URLs on WebView2 (native_webview.cpp).
// Pure (no Windows call, no JNI), so test/webview_file_urls_test.cpp checks it
// on its own.
//
// WebView2 cannot serve an APK's assets, and file: requests never reach
// WebResourceRequested, so the app's file:///X is loaded as kFileOrigin + "/X"
// and every request there is answered by the runtime: the app's
// shouldInterceptRequest first, then MacApkWebViewBridge.macapkServeFile
// (file:///android_asset/ from the APK's assets, file:///android_res/ from its
// resources, other paths from the app's files when WebSettings allows). The app
// sees file:///X everywhere (getUrl, WebViewClient's callbacks,
// WebResourceRequest, dialogs); the page sees an https origin
// (location.protocol is "https:"), and its file: pages share one origin, as
// with setAllowFileAccessFromFileURLs.
//
//   file:///android_asset/www/index.html?cordova
//       <-> https://winapk-file.example/android_asset/www/index.html?cordova
//
// A device's WebView lets only file: pages and the app's own
// loadDataWithBaseURL pages load file: URLs; a web page gets "Not allowed to
// load local resource". Any page in the view can name the origin, so
// file_request_allowed keeps that rule: the top document must be one of those
// (`file_ok`), and a request whose Origin or Referer is another web origin
// (other than the data page's own base) is refused.
//
// The design (one synthetic https origin, a deferred WebResourceRequested, the
// app's URLs mapped both ways, a data page's file:/// references made the
// origin's) is adapted from the other runtime (https://example.invalid/other-runtime),
// app/java/com/other/app/webview/FileUrls.java and
// compat/webview/webview_host.cc, Copyright (C) 2026 its authors,
// SPDX-License-Identifier: Apache-2.0. Modified for Win.apk.

#pragma once

#include <cwctype>
#include <string>

namespace winapk::host::wvfile {

constexpr wchar_t kFileOrigin[] = L"https://winapk-file.example";
constexpr size_t kFileOriginLen = sizeof(kFileOrigin) / sizeof(wchar_t) - 1;

inline bool starts_with_nocase(const std::wstring& s, const wchar_t* prefix, size_t n) {
    if (s.size() < n) return false;
    for (size_t k = 0; k < n; ++k) {
        if (towlower(s[k]) != towlower(prefix[k])) return false;
    }
    return true;
}

// The URL's scheme, lower case ("" when it has none).
inline std::wstring scheme_of(const std::wstring& url) {
    const size_t colon = url.find(L':');
    if (colon == std::wstring::npos || colon == 0) return std::wstring();
    std::wstring s = url.substr(0, colon);
    for (wchar_t& c : s) {
        if (!(iswalnum(c) || c == L'+' || c == L'-' || c == L'.')) return std::wstring();
        c = static_cast<wchar_t>(towlower(c));
    }
    return s;
}

// Is u on the file origin (the origin itself, or a URL under it)?
inline bool on_file_origin(const std::wstring& u) {
    if (!starts_with_nocase(u, kFileOrigin, kFileOriginLen)) return false;
    return u.size() == kFileOriginLen || u[kFileOriginLen] == L'/' || u[kFileOriginLen] == L'?' ||
           u[kFileOriginLen] == L'#';
}

// The URL WebView2 loads for an app's URL: file:///X -> the file origin's /X.
// file://host/X (a share) is left alone: WebView2 reads it itself.
inline std::wstring engine_url(const std::wstring& u) {
    if (starts_with_nocase(u, L"file:///", 8)) return kFileOrigin + u.substr(7);
    return u;
}

// The app's URL for one WebView2 reports.
inline std::wstring guest_url(const std::wstring& u) {
    if (!on_file_origin(u)) return u;
    std::wstring rest = u.substr(kFileOriginLen);
    if (rest.empty() || rest[0] != L'/') rest = L"/" + rest;
    return L"file://" + rest;
}

// "scheme://host[:port]" of a web URL, lower case; empty for anything else
// (an Origin header's "null", about:, data:).
inline std::wstring origin_of(const std::wstring& u) {
    const std::wstring scheme = scheme_of(u);
    if (scheme != L"http" && scheme != L"https") return std::wstring();
    const size_t start = scheme.size() + 3;
    if (u.size() < start || u.compare(scheme.size(), 3, L"://") != 0) return std::wstring();
    std::wstring o = u.substr(0, u.find_first_of(L"/?#", start));
    for (wchar_t& c : o) c = static_cast<wchar_t>(towlower(c));
    return o;
}

// A loadDataWithBaseURL page's file:/// references (src="file:///android_asset/x.js",
// url('file:///...')), which Android's page loads, made the file origin's: in
// WebView2 a file: URL in an https page loads nothing. Only where an attribute
// value, a string or url( starts, so the page's text keeps a file:/// it shows.
inline std::wstring rewrite_file_refs(const std::wstring& data) {
    static const wchar_t kFile[] = L"file:///";
    std::wstring out;
    size_t from = 0;
    for (;;) {
        const size_t at = data.find(kFile, from);
        if (at == std::wstring::npos) break;
        const wchar_t before = at > 0 ? data[at - 1] : L'\0';
        out.append(data, from, at - from);
        if (before == L'"' || before == L'\'' || before == L'(' || before == L'=') {
            out += kFileOrigin;
            out += L'/';
        } else {
            out += kFile;
        }
        from = at + 8;
    }
    out.append(data, from, std::wstring::npos);
    return out;
}

// The URL WebView2 requests for a navigation to u: no fragment, and "/" for an
// empty path, so a loadDataWithBaseURL base matches its document's request.
inline std::wstring request_form(const std::wstring& u) {
    std::wstring r = u.substr(0, u.find(L'#'));
    const std::wstring scheme = scheme_of(r);
    if (scheme != L"http" && scheme != L"https") return r;
    const size_t cut = r.find_first_of(L"/?", scheme.size() + 3);
    if (cut == std::wstring::npos) r += L"/";
    else if (r[cut] == L'?') r.insert(cut, L"/");
    return r;
}

// May a request on the file origin be answered? `file_ok`: the top document is
// a file: page or the app's loadDataWithBaseURL page; `data_origin`: that data
// page's base origin, when it is a web URL; `origin` and `referer`: the
// request's headers, empty when absent.
inline bool file_request_allowed(bool file_ok, const std::wstring& data_origin, const std::wstring& origin,
                                 const std::wstring& referer) {
    if (!file_ok) return false;
    for (const std::wstring* h : {&origin, &referer}) {
        const std::wstring from = origin_of(*h);
        if (from.empty() || on_file_origin(from) || (!data_origin.empty() && from == data_origin)) continue;
        return false;
    }
    return true;
}

}  // namespace winapk::host::wvfile
