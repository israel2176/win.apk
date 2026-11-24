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
// Beside the tracker, the rules a release and its installer are held to before the installer runs (adapted from
// the other runtime's updater, below): which release of GitHub's list is offered (pick_release), whether its installer may be
// downloaded at all (installer_checkable: GitHub's SHA-256 digest of it is required, a size alone is not enough),
// and whether a downloaded or kept installer may run (judge_setup: its size, its SHA-256, its Authenticode
// signature and its product version).
//
// The release rules are adapted from the other runtime (https://example.invalid/other-runtime),
// tools/package/other-update.ps1 (ConvertTo-the other runtimeUpdateRelease, Assert-the other runtimeUpdateInstaller),
// Copyright (C) 2026 its authors, SPDX-License-Identifier: Apache-2.0. Modified for Win.apk.

#pragma once

#include <algorithm>
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
struct ListedRelease {
    std::wstring tag;
    bool draft = false, prerelease = false;
    bool asset = false;       // the release has the installer asset
    std::string asset_state;  // GitHub's "state": "uploaded" once the upload is whole
    std::string digest;       // GitHub's "digest": "sha256:<64 hex>"
    uint64_t size = 0;        // the asset's bytes
};

// The lower-case hex of a "sha256:<64 hex>" digest; empty when it is not one.
inline std::string asset_sha256(const std::string& digest) {
    if (digest.size() != 7 + 64 || digest.compare(0, 7, "sha256:") != 0) return {};
    std::string hex = digest.substr(7);
    for (char& c : hex) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
    }
    return hex;
}

// The largest installer Win.apk downloads: anything bigger is not one of its installers.
constexpr uint64_t kMaxSetupBytes = 2ull << 30;

// Whether the release's installer may be downloaded: uploaded whole, of a sane size, with GitHub's SHA-256 of it
// to check the download against. A release without one is still shown, with its page instead of Update.
inline bool installer_checkable(const ListedRelease& r) {
    return r.asset && r.asset_state == "uploaded" && r.size > 0 && r.size <= kMaxSetupBytes &&
           !asset_sha256(r.digest).empty();
}

// The release to offer, an index into `list`, or -1: the highest version that is not a draft. Pre-releases count
// only while the list has no full release: stable builds are offered stable releases, as the other runtime's updater does,
// but every Win.apk release so far is a pre-release (windows-release.yml publishes with --prerelease), and an
// updater that skipped them all would never offer anything.
inline int pick_release(const std::vector<ListedRelease>& list) {
    auto usable = [](const ListedRelease& r) { return !r.draft && !version_numbers(r.tag).empty(); };
    bool any_stable = false;
    for (const ListedRelease& r : list) any_stable = any_stable || (usable(r) && !r.prerelease);
    int best = -1;
    for (size_t i = 0; i < list.size(); ++i) {
        const ListedRelease& r = list[i];
        if (!usable(r) || (any_stable && r.prerelease)) continue;
        if (best < 0 || compare_versions(r.tag, list[static_cast<size_t>(best)].tag) > 0) best = static_cast<int>(i);
    }
    return best;
}

// An installer's Authenticode signature, as WinVerifyTrust judged it.
enum class Signature { kUnsigned, kValid, kInvalid };

// What was found of an installer on disk.
struct SetupFacts {
    uint64_t size = 0;
    std::string sha256;            // lower-case hex
    std::wstring product_version;  // its version resource's ProductVersion string
    Signature signature = Signature::kUnsigned;
    std::wstring signer;           // the signing certificate's name, when kValid
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

inline bool setup_runs(SetupVerdict v) { return v == SetupVerdict::kOk || v == SetupVerdict::kOkUnsigned; }

// Whether an installer may run as the update to `version`, whose asset GitHub lists with `size` bytes and the
// SHA-256 `sha256`. `own_signer`: the name on this build's own signature, empty when it has none. A signed build
// takes only an installer signed by the same name; an unsigned build takes an unsigned installer on its SHA-256,
// but never one whose signature is broken. Inno Setup pads ProductVersion with spaces; they are not part of it.
inline SetupVerdict judge_setup(const SetupFacts& f, uint64_t size, const std::string& sha256,
                                const std::wstring& version, const std::wstring& own_signer) {
    if (f.size != size || size == 0) return SetupVerdict::kSize;
    if (sha256.size() != 64 || f.sha256 != sha256) return SetupVerdict::kDigest;
    if (f.signature == Signature::kInvalid) return SetupVerdict::kBadSignature;
    if (!own_signer.empty() && f.signature != Signature::kValid) return SetupVerdict::kNotSigned;
    if (!own_signer.empty() && f.signer != own_signer) return SetupVerdict::kSignerChanged;
    std::wstring pv = f.product_version;
    while (!pv.empty() && (pv.back() == L' ' || pv.back() == L'\0')) pv.pop_back();
    if (!pv.empty() && (pv[0] == L'v' || pv[0] == L'V')) pv.erase(0, 1);
    std::wstring want = version;
    if (!want.empty() && (want[0] == L'v' || want[0] == L'V')) want.erase(0, 1);
    if (want.empty() || pv != want) return SetupVerdict::kVersion;
    return f.signature == Signature::kValid ? SetupVerdict::kOk : SetupVerdict::kOkUnsigned;
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
