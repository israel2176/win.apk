// webview_file_urls.h -- the app's file: URLs on WebView2 (native_webview.cpp). Pure (no Windows call, no JNI), so
// test/webview_file_urls_test.cpp checks it on its own.
//
// On a device the WebView answers file:///android_asset/ and file:///android_res/ itself (Chromium's
// AndroidProtocolHandler: the APK's assets and resources), and other file: paths from the disk when WebSettings
// allows. WebView2 reads no APK and raises WebResourceRequested only for http(s), so the app's file: URLs are loaded
// on a host of their own, the "file origin", the way androidx's WebViewAssetLoader puts an app's assets on
// https://appassets.androidplatform.net: a secure origin of one name, never a real host, every request to which is
// answered in-process (WebView2's WebResourceRequested with a deferral, the answer made on a worker thread):
//
//   file:///android_asset/www/index.html?cordova  <->  https://winapk-file.example/android_asset/www/index.html?cordova
//
// The app sees its file: URLs everywhere (getUrl, WebViewClient's callbacks, WebResourceRequest); the page sees an
// https origin, and its file: pages share it, as with setAllowFileAccessFromFileURLs.
//
// Who may read it follows what a device's WebView allows: file: URLs load in a file: page and in the app's own
// loadDataWithBaseURL page, while a web page gets "Not allowed to load local resource". Since any page could name the
// origin, file_request_allowed holds every request to that, by the top document and the request's Origin and
// Referer. And as on any https origin, what it serves is same-origin only: no CORS header, except to the opaque
// origin of the app's own data page (native_webview.cpp).
//
// Media seeks by Range requests; the answer is a whole body from Java, so a single byte range of it is cut here
// (byte_range, RFC 9110 section 14) and served as 206 Partial Content, as a web server would.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <string>

namespace winapk::host::wvfile {

constexpr wchar_t kFileOrigin[] = L"https://winapk-file.example";

// Does s, from `at`, start with the ASCII text `lit`, in any case?
inline bool has_at(const std::wstring& s, size_t at, const wchar_t* lit) {
    for (size_t i = 0; lit[i]; ++i) {
        if (at + i >= s.size() || std::towlower(s[at + i]) != static_cast<wint_t>(lit[i])) return false;
    }
    return true;
}

inline size_t lit_len(const wchar_t* lit) {
    size_t n = 0;
    while (lit[n]) ++n;
    return n;
}

inline std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// Where the authority of a URL starting "scheme://" ends: at its path, query or fragment, or the end.
inline size_t authority_end(const std::wstring& u, size_t from) {
    size_t i = from;
    while (i < u.size() && u[i] != L'/' && u[i] != L'?' && u[i] != L'#') ++i;
    return i;
}

// Where the file origin's part of u ends (just past its host, or ":443"), or npos when u is not on it.
inline size_t file_origin_end(const std::wstring& u) {
    if (!has_at(u, 0, kFileOrigin)) return std::wstring::npos;
    size_t i = lit_len(kFileOrigin);
    if (has_at(u, i, L":443")) i += 4;  // the default port spelled out is the same origin
    if (i == u.size() || u[i] == L'/' || u[i] == L'?' || u[i] == L'#') return i;
    return std::wstring::npos;  // a longer host name, or another port
}

// Is u on the file origin: the origin itself, or a URL under it?
inline bool on_file_origin(const std::wstring& u) { return file_origin_end(u) != std::wstring::npos; }

// The URL WebView2 loads for an app's URL. A local file: URL (file:///X, file://localhost/X, which RFC 8089 makes
// the same, or file:/X) becomes the file origin's /X, query and fragment kept. file://host/X, a share, is left to
// WebView2, which reads it itself; anything else is left alone.
inline std::wstring engine_url(const std::wstring& u) {
    if (!has_at(u, 0, L"file:")) return u;
    size_t path;
    if (has_at(u, 5, L"///")) {
        path = 7;
    } else if (has_at(u, 5, L"//localhost/")) {
        path = 16;
    } else if (has_at(u, 5, L"/") && !has_at(u, 5, L"//")) {
        path = 5;
    } else {
        return u;
    }
    return std::wstring(kFileOrigin) + u.substr(path);
}

// The app's URL for one WebView2 reports: the file origin's URLs as file:///, the rest as they are.
inline std::wstring guest_url(const std::wstring& u) {
    const size_t end = file_origin_end(u);
    if (end == std::wstring::npos) return u;
    std::wstring rest = u.substr(end);
    if (rest.empty() || rest[0] != L'/') rest.insert(rest.begin(), L'/');
    return L"file://" + rest;
}

// The origin of a web URL as a browser serializes it (HTML's "ASCII serialization of an origin"): scheme and host in
// lower case, the port only when it is not the scheme's default, no user info. Empty for anything without one
// (an Origin header's "null", about:, data:, file:).
inline std::wstring origin_of(const std::wstring& u) {
    static const struct {
        const wchar_t* scheme;
        const wchar_t* default_port;
    } kWeb[] = {{L"https", L"443"}, {L"http", L"80"}, {L"wss", L"443"}, {L"ws", L"80"}};
    for (const auto& w : kWeb) {
        const size_t n = lit_len(w.scheme);
        if (!has_at(u, 0, w.scheme) || !has_at(u, n, L"://")) continue;
        const size_t from = n + 3, to = authority_end(u, from);
        std::wstring host = u.substr(from, to - from);
        const size_t at = host.rfind(L'@');
        if (at != std::wstring::npos) host.erase(0, at + 1);
        if (host.empty()) return {};
        const size_t colon = host.rfind(L':');
        const size_t bracket = host.rfind(L']');  // an IPv6 address's colons are not a port's
        if (colon != std::wstring::npos && (bracket == std::wstring::npos || colon > bracket)) {
            const std::wstring port = host.substr(colon + 1);
            if (port.empty() || port == w.default_port) host.erase(colon);
        }
        return std::wstring(w.scheme) + L"://" + lower(host);
    }
    return {};
}

// A loadDataWithBaseURL page's file:/// references, which load on a device, made the file origin's: in WebView2 a
// file: URL in an https page loads nothing. Only where a value starts, after a quote, after = in an unquoted
// attribute, or inside url(, so that text the page shows keeps the file:/// it says.
inline std::wstring rewrite_file_refs(const std::wstring& data) {
    static const wchar_t kRef[] = L"file:///";
    std::wstring out;
    out.reserve(data.size());
    size_t copied = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        if (!has_at(data, i, kRef)) continue;
        size_t j = i;
        while (j > 0 && (data[j - 1] == L' ' || data[j - 1] == L'\t' || data[j - 1] == L'\n' || data[j - 1] == L'\r'))
            --j;
        const wchar_t before = i > 0 ? data[i - 1] : 0;
        const wchar_t token = j > 0 ? data[j - 1] : 0;
        const bool starts_value = before == L'"' || before == L'\'' || token == L'=' ||
                                  (token == L'(' && j >= 4 && has_at(data, j - 4, L"url("));
        if (!starts_value) continue;
        out.append(data, copied, i - copied);
        out += kFileOrigin;
        out += L'/';
        copied = i + lit_len(kRef);
        i = copied - 1;
    }
    out.append(data, copied, std::wstring::npos);
    return out;
}

// The URL WebView2 requests for a navigation to u: without its fragment (never sent), and with "/" for an empty
// path, so that a loadDataWithBaseURL base matches its document's request.
inline std::wstring request_form(const std::wstring& u) {
    std::wstring r = u.substr(0, u.find(L'#'));
    const size_t sep = r.find(L"://");
    if (sep == std::wstring::npos) return r;
    const size_t end = authority_end(r, sep + 3);
    if (end == r.size() || r[end] != L'/') r.insert(end, 1, L'/');
    return r;
}

// May a request on the file origin be answered? `file_ok`: the top document is a file: page or the app's
// loadDataWithBaseURL page; `data_origin`: that data page's base origin, when it is a web URL; `origin` and
// `referer`: the request's headers, empty when absent. Both headers, when sent, must name the file origin, the data
// page's own base, or (Origin: null) the data page's opaque origin; a frame or a script of another web origin
// inside an allowed page is refused as a web page would be.
inline bool file_request_allowed(bool file_ok, const std::wstring& data_origin, const std::wstring& origin,
                                 const std::wstring& referer) {
    if (!file_ok) return false;
    auto ours = [&](const std::wstring& o) {
        return o == kFileOrigin || (!data_origin.empty() && o == data_origin);
    };
    if (!origin.empty() && origin != L"null" && !ours(origin_of(origin))) return false;
    if (!referer.empty() && !on_file_origin(referer) && !ours(origin_of(referer))) return false;
    return true;
}

// One byte range of a body of `size` bytes, from a Range header (RFC 9110 section 14.2). kWhole: no range, or
// one this server may ignore (another unit, a syntax error, several ranges): the whole body, 200. kPart: the
// bytes [first, last], 206. kUnsatisfiable: no byte of the range is in the body, 416.
struct ByteRange {
    enum Kind { kWhole, kPart, kUnsatisfiable } kind = kWhole;
    uint64_t first = 0, last = 0;
};

inline ByteRange byte_range(const std::wstring& header, uint64_t size) {
    ByteRange r;
    size_t i = 0;
    auto skip_ws = [&] {
        while (i < header.size() && (header[i] == L' ' || header[i] == L'\t')) ++i;
    };
    auto number = [&](uint64_t* out) {
        const size_t from = i;
        uint64_t n = 0;
        while (i < header.size() && header[i] >= L'0' && header[i] <= L'9') {
            if (n > (UINT64_MAX - 9) / 10) return false;
            n = n * 10 + static_cast<uint64_t>(header[i++] - L'0');
        }
        *out = n;
        return i > from;
    };
    skip_ws();
    if (!has_at(header, i, L"bytes")) return r;
    i += 5;
    skip_ws();
    if (i >= header.size() || header[i] != L'=') return r;
    ++i;
    skip_ws();
    uint64_t first = 0, last = 0;
    bool suffix = false, open_end = false;
    if (i < header.size() && header[i] == L'-') {
        ++i;
        suffix = true;
        if (!number(&last)) return r;
    } else {
        if (!number(&first)) return r;
        if (i >= header.size() || header[i] != L'-') return r;
        ++i;
        open_end = !number(&last);
        if (!open_end && last < first) return r;  // invalid: the whole header is ignored
    }
    skip_ws();
    if (i != header.size()) return r;  // several ranges, or junk: the whole body
    if (suffix) {
        if (last == 0 || size == 0) {
            r.kind = ByteRange::kUnsatisfiable;
            return r;
        }
        r.kind = ByteRange::kPart;
        r.first = last >= size ? 0 : size - last;
        r.last = size - 1;
        return r;
    }
    if (first >= size) {
        r.kind = ByteRange::kUnsatisfiable;
        return r;
    }
    r.kind = ByteRange::kPart;
    r.first = first;
    r.last = open_end || last >= size ? size - 1 : last;
    return r;
}

}  // namespace winapk::host::wvfile
