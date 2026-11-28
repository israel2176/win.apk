// updates.h -- where the update check and the installer's download stand.
//
// Everything here happens on the library's thread: the check and the download run on threads of their
// own (update_check_thread, update_download_thread) that only post their results (kUpdateChecked,
// kUpdateProgress, kUpdateDownloaded). So there is no lock; UpdateTracker makes no Windows call either,
// so that test/launcher_state_test.cpp can drive it.
//
// Three things move on their own, and each has its own states:
//
//   the check     kIdle -> kReadingKept | kAsking     begin_check(): at start, daily, "Check"
//                 kReadingKept, kAsking -> kIdle      checked(): kUpdateChecked; abort_check(): no thread
//                 One check at a time: begin_check() refuses while one runs.
//
//   what is known kNothing -> kNoRelease | kRelease   checked(), when the list of releases was read
//                 kNoRelease <-> kRelease             the same, at a later check
//                 A check that read nothing leaves what was known as it was. Beside it: whether the
//                 last check that asked GitHub failed, and whether the InfoBar has told of this release.
//
//   the download  not downloading -> 0..99 %          begin_download(): "Update"
//                 n % -> m %                          progress(): kUpdateProgress
//                 n % -> not downloading              downloaded(): kUpdateDownloaded
//
// checked() also answers what the InfoBar says about the check, so that the rule is in one place.
//
// Beside the tracker, the rules a release and its installer are held to before the installer runs: which release
// of GitHub's list is offered (pick_release), whether its installer may be downloaded at all (installer_checkable:
// GitHub's SHA-256 digest of it is required, a size alone is not enough), and whether a downloaded or kept
// installer may run (judge_setup: its size, its SHA-256, its Authenticode signature and its product version). They
// read the fields GitHub's REST API documents for a release and its assets; the facts about the file on disk are
// gathered by the launcher (WinVerifyTrust, GetFileVersionInfo, BCrypt's SHA-256).

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace winapk::launcher {

// 0.2.0 of "v0.2.0-preview": the numbers before any -suffix or +build.
inline std::vector<unsigned> version_numbers(const std::wstring& v) {
    std::vector<unsigned> out;
    size_t i = !v.empty() && (v[0] == L'v' || v[0] == L'V') ? 1 : 0;
    while (i < v.size() && iswdigit(v[i])) {
        unsigned n = 0;
        while (i < v.size() && iswdigit(v[i])) n = n * 10 + static_cast<unsigned>(v[i++] - L'0');
        out.push_back(n);
        if (i >= v.size() || v[i] != L'.') break;
        ++i;
    }
    return out;
}

// <0, 0, >0 as a is older than, the same as or newer than b; missing numbers count as 0 (0.2 = 0.2.0).
inline int compare_versions(const std::wstring& a, const std::wstring& b) {
    const std::vector<unsigned> x = version_numbers(a), y = version_numbers(b);
    for (size_t i = 0; i < (std::max)(x.size(), y.size()); ++i) {
        const unsigned p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}
// One release of GitHub's list, as far as choosing it goes, with its Win.apk-Setup.exe asset when it has one.
// The fields are the REST API's ("List releases", GET /repos/{owner}/{repo}/releases): a release's `tag_name`,
// `draft` and `prerelease`, and of its asset (`assets[]`) `state`, `size` and `digest`.
struct ListedRelease {
    std::wstring tag;
    bool draft = false, prerelease = false;
    bool asset = false;       // the release has the installer asset
    std::string asset_state;  // the asset's "state": "uploaded" once whole, "open" while an upload runs
    std::string digest;       // the asset's "digest": "<algorithm>:<hex>", GitHub's hash of what it stores
    uint64_t size = 0;        // the asset's bytes
};

// The largest installer believed: Win.apk-Setup.exe is a few hundred MB; GitHub takes assets up to 2 GiB.
constexpr uint64_t kMaxSetupBytes = 2ull << 30;

// The SHA-256 in an asset's digest, as 64 lower-case hex digits; empty when the digest is of another algorithm, or
// malformed. GitHub writes "sha256:" and lower case; any case is read.
inline std::string asset_sha256(const std::string& digest) {
    static const char kPrefix[] = "sha256:";
    constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (digest.size() != kPrefixLen + 64) return {};
    for (size_t i = 0; i < kPrefixLen; ++i)
        if (std::tolower(static_cast<unsigned char>(digest[i])) != kPrefix[i]) return {};
    std::string hex;
    hex.reserve(64);
    for (size_t i = kPrefixLen; i < digest.size(); ++i) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(digest[i])));
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
        hex += c;
    }
    return hex;
}

// Whether a release's installer may be downloaded at all: it is there, its upload finished, its size is believable,
// and GitHub gives its SHA-256, which the download is then held to. A size alone proves nothing about the bytes, so
// a release with no digest (assets uploaded before GitHub kept them) is offered by its page, not downloaded.
inline bool installer_checkable(const ListedRelease& r) {
    if (!r.asset || r.asset_state != "uploaded") return false;
    if (r.size == 0 || r.size > kMaxSetupBytes) return false;
    return !asset_sha256(r.digest).empty();
}

// The index in `list` of the release to offer, or -1. Drafts never (the API lists them only to the repository's
// writers, and they are not published). Among the rest, only full releases while there is one, else only
// pre-releases: a build is never offered a pre-release once a full release exists. The highest version number
// wins, whatever the list's order (GitHub sorts by creation date, not version); on equal numbers the one listed
// first, the newer. A tag with no version number is no release.
inline int pick_release(const std::vector<ListedRelease>& list) {
    bool any_full = false;
    for (const ListedRelease& r : list)
        if (!r.draft && !r.prerelease && !version_numbers(r.tag).empty()) any_full = true;
    int best = -1;
    for (size_t i = 0; i < list.size(); ++i) {
        const ListedRelease& r = list[i];
        if (r.draft || version_numbers(r.tag).empty()) continue;
        if (any_full && r.prerelease) continue;
        if (best < 0 || compare_versions(r.tag, list[static_cast<size_t>(best)].tag) > 0) best = static_cast<int>(i);
    }
    return best;
}

// An installer's Authenticode signature, as WinVerifyTrust judges it.
enum class Signature { kUnsigned, kValid, kInvalid };

// What was found of an installer on disk.
struct SetupFacts {
    uint64_t size = 0;
    std::string sha256;            // lower-case hex
    std::wstring product_version;  // its version resource's ProductVersion string
    Signature signature = Signature::kUnsigned;
    std::wstring signer;           // who signed it, when kValid (the launcher's authenticode())
};

enum class SetupVerdict {
    kOk,             // whole, the release's, and signed as this build is
    kOkUnsigned,     // whole and the release's by its SHA-256, but not code-signed (nor is this build)
    kSize,           // not the release's size: cut off or something else
    kDigest,         // not the SHA-256 GitHub lists
    kBadSignature,   // signed, but the signature does not verify
    kNotSigned,      // this build is code-signed and the installer is not
    kSignerChanged,  // signed by someone other than this build's signer
    kVersion,        // its product version is not the release's
};

inline bool setup_runs(SetupVerdict v) { return v == SetupVerdict::kOk; }

// A version as written in a tag or a version resource, compared as text: without a leading v and without the
// blanks and NULs around it (Inno Setup pads ProductVersion with spaces; a resource string may keep its NUL).
inline std::wstring plain_version(const std::wstring& v) {
    size_t b = 0, e = v.size();
    auto blank = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\0'; };
    while (b < e && blank(v[b])) ++b;
    while (e > b && blank(v[e - 1])) --e;
    if (b < e && (v[b] == L'v' || v[b] == L'V')) ++b;
    return v.substr(b, e - b);
}

// Whether an installer may run as the update to `version`, whose asset GitHub lists with `size` bytes and the
// SHA-256 `sha256`. `own_signer`: who signed this build, empty when nobody did. The checks go from the cheapest
// to the one that matters most for trust: the size (a cut-off download), the SHA-256 (the very bytes GitHub
// serves), the signature (a broken one is refused; both this build and the installer must be validly signed by
// the same signer), then the version inside (an older installer replayed under a newer release's name).
inline SetupVerdict judge_setup(const SetupFacts& f, uint64_t size, const std::string& sha256,
                                const std::wstring& version, const std::wstring& own_signer) {
    if (size == 0 || f.size != size) return SetupVerdict::kSize;
    if (sha256.size() != 64 || f.sha256 != sha256) return SetupVerdict::kDigest;
    if (f.signature == Signature::kInvalid) return SetupVerdict::kBadSignature;
    if (own_signer.empty() || f.signature != Signature::kValid) return SetupVerdict::kNotSigned;
    if (f.signer != own_signer) return SetupVerdict::kSignerChanged;
    const std::wstring want = plain_version(version);
    if (want.empty() || plain_version(f.product_version) != want) return SetupVerdict::kVersion;
    return SetupVerdict::kOk;
}

// What a response's body must come to, so that a download cut off is never kept as whole (the launcher's
// http_get, for every download). From HTTP's framing rules (RFC 9110 section 8.6, RFC 9112 section 6.3):
//   - with a Transfer-Encoding the body is framed by it (chunked: WinHTTP fails the read of a body cut short) and
//     any Content-Length is ignored;
//   - Content-Length is one or more decimal digits; a header repeated, or a list, counts only when every value is
//     the same number, and anything else makes the response unusable (a recipient must not guess);
//   - a body under a Content-Encoding other than identity that the client inflates (`decoded`) comes to some other
//     number of bytes than the length, which then counts for nothing.
struct BodyLength {
    enum Kind { kUnknown, kExact, kBroken } kind = kUnknown;
    uint64_t bytes = 0;  // when kExact
    bool exact() const { return kind == kExact; }
    // Whether `got` bytes are the whole body.
    bool whole(uint64_t got) const { return kind == kUnknown || (kind == kExact && got == bytes); }
};

inline BodyLength expected_body(const std::wstring& content_length, const std::wstring& content_encoding,
                                const std::wstring& transfer_encoding, bool decoded) {
    auto trim = [](const std::wstring& s, size_t b, size_t e) {
        while (b < e && (s[b] == L' ' || s[b] == L'\t')) ++b;
        while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t')) --e;
        return s.substr(b, e - b);
    };
    BodyLength out;
    if (!trim(transfer_encoding, 0, transfer_encoding.size()).empty()) return out;
    if (trim(content_length, 0, content_length.size()).empty()) return out;
    bool first = true;
    uint64_t n = 0;
    for (size_t b = 0; b <= content_length.size();) {
        size_t e = content_length.find(L',', b);
        if (e == std::wstring::npos) e = content_length.size();
        const std::wstring v = trim(content_length, b, e);
        uint64_t x = 0;
        bool ok = !v.empty() && v.size() <= 19;  // 19 digits always fit in 64 bits
        for (size_t i = 0; ok && i < v.size(); ++i) {
            ok = v[i] >= L'0' && v[i] <= L'9';
            x = x * 10 + static_cast<unsigned>(v[i] - L'0');
        }
        if (!ok || (!first && x != n)) {
            out.kind = BodyLength::kBroken;
            return out;
        }
        n = x;
        first = false;
        b = e + 1;
    }
    const std::wstring encoding = trim(content_encoding, 0, content_encoding.size());
    bool identity = encoding.size() == 8;
    for (size_t i = 0; identity && i < 8; ++i)
        identity = static_cast<wchar_t>(std::towlower(encoding[i])) == L"identity"[i];
    if (decoded && !encoding.empty() && !identity) return out;
    out.kind = BodyLength::kExact;
    out.bytes = n;
    return out;
}

// One check's result, as its thread reports it.
struct CheckResult {
    bool fetch = false;    // GitHub was asked; else only the answer kept from the last check was read
    bool manual = false;   // "Check": the result is said in the InfoBar whatever it is
    bool reached = true;   // GitHub answered (a fetch)
    bool ok = false;       // the list of releases was read, GitHub's answer or the one kept
    bool found = false;    // and it has a release
};

// `Release` is the launcher's: all that is asked of it here is `.version`.
template <class Release>
class UpdateTracker {
public:
    enum class Check { kIdle, kReadingKept, kAsking };
    enum class Known { kNothing, kNoRelease, kRelease };
    enum class Say { kNothing, kCouldNotCheck, kUpToDate, kNewVersion };

    // A check starts. False, changing nothing, while one runs.
    bool begin_check(bool fetch) {
        if (check_ != Check::kIdle) return false;
        check_ = fetch ? Check::kAsking : Check::kReadingKept;
        return true;
    }
    // Its thread could not be started.
    void abort_check() { check_ = Check::kIdle; }

    // The check is back. `latest` is the newest release when r.found, and `newer` whether it is newer
    // than this build; `auto_on`: the daily check is on. Returns what to say in the InfoBar.
    Say checked(const CheckResult& r, Release latest, bool newer, bool auto_on) {
        check_ = Check::kIdle;
        if (r.ok) {
            // Another release than the one told of: it is news again.
            if (r.found && (known_ != Known::kRelease || latest_.version != latest.version)) told_ = false;
            if (r.found) {
                latest_ = std::move(latest);
                newer_ = newer;
            }
            known_ = r.found ? Known::kRelease : Known::kNoRelease;
        }
        if (r.fetch) fetch_failed_ = !r.reached || !r.ok;
        if (r.manual && fetch_failed_) return Say::kCouldNotCheck;
        if (r.manual && !available()) return Say::kUpToDate;
        if (available() && !told_ && (r.manual || auto_on)) {
            told_ = true;
            return Say::kNewVersion;
        }
        return Say::kNothing;
    }

    // The installer's download starts. False when one runs, or there is nothing newer to download.
    bool begin_download() {
        if (download_ || !available()) return false;
        download_ = 0;
        return true;
    }
    void progress(int percent) {
        if (download_) download_ = percent;
    }
    void downloaded() { download_.reset(); }

    Check check() const { return check_; }
    bool asking() const { return check_ == Check::kAsking; }  // GitHub is being asked
    Known known() const { return known_; }
    bool fetch_failed() const { return fetch_failed_; }  // the last check that asked GitHub got no list
    // A release newer than this build is known.
    bool available() const { return known_ == Known::kRelease && newer_; }
    const Release& latest() const { return latest_; }  // the newest release last found; empty before any
    bool downloading() const { return download_.has_value(); }
    int percent() const { return download_.value_or(-1); }

private:
    Check check_ = Check::kIdle;
    Known known_ = Known::kNothing;
    bool newer_ = false;         // latest_ is newer than this build
    bool fetch_failed_ = false;
    bool told_ = false;          // the InfoBar has said this session that latest_ is out
    std::optional<int> download_;  // Win.apk-Setup.exe is downloading: how far
    Release latest_;
};

}  // namespace winapk::launcher
