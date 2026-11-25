// store_app.h -- where the Store, Win.apk's own store app, stands: its newest release read from GitHub, the APK
// downloaded and checked, installed, and kept up to date.
//
// The Store item in the navigation pane shows the store app (israel2176/win.apk-store, package app.apkstore)
// inside the library window. Win.apk gets it as a system component, as a device comes with its store: on the
// first opening of the Store item, and when the daily update check finds a newer release, the release's
// apk-store.apk is downloaded, held to GitHub's size and SHA-256 of it, and installed quietly. Everything here
// happens on the library's thread; the release check and the download run on threads of their own that only post
// their results. So there is no lock, and StoreSetup makes no Windows call, so that test/launcher_state_test.cpp
// drives it.
//
//   kIdle -> kChecking                 begin_check(): the Store item opened with no store app, or the daily check
//   kChecking -> kDownloading          checked(): a checkable release whose APK is not the one installed
//   kChecking -> kIdle | kFailed       checked(): nothing newer (or nothing to check, with the store installed)
//   kDownloading -> kInstalling        downloaded(): the file is the release's, and the store app is not running
//   kDownloading -> kWaiting           downloaded(): the same, while the store app runs (it is in use)
//   kWaiting -> kInstalling            may_install(): the store app no longer runs
//   kDownloading -> kFailed            downloaded(): broke off, or not the release's file (deleted, never installed)
//   kInstalling -> kIdle | kFailed     installed()
//   kFailed -> kChecking               begin_check() again (Try again)
//
// A failure is shown only while there is no store app to show (`installed` false at the step): with one
// installed, a check or an update that does not go through leaves the installed store as it is.

#pragma once

#include <cstdint>
#include <string>

#include "updates.h"

namespace winapk::launcher {

// The store app's package and its release asset's name.
constexpr char kStorePackage[] = "app.apkstore";
constexpr wchar_t kStoreAsset[] = L"apk-store.apk";

// The largest store APK believed: it is a few MB; GitHub takes assets up to 2 GiB.
constexpr uint64_t kMaxStoreBytes = 512ull << 20;

// Whether the release's apk-store.apk may be downloaded at all: as an installer (updates.h installer_checkable),
// it is there, its upload finished, and GitHub gives its SHA-256; and it is of a believable size.
inline bool store_asset_checkable(const ListedRelease& r) {
    return installer_checkable(r) && r.size <= kMaxStoreBytes;
}

// A downloaded apk-store.apk, against the release: kept only at the release's size and SHA-256.
enum class StoreFile { kOk, kWrongSize, kWrongSha256 };

inline StoreFile judge_store_file(uint64_t size, const std::string& sha256, uint64_t want_size,
                                  const std::string& want_sha256) {
    if (size == 0 || size != want_size) return StoreFile::kWrongSize;
    if (sha256.empty() || sha256 != want_sha256) return StoreFile::kWrongSha256;
    return StoreFile::kOk;
}

class StoreSetup {
public:
    enum class Step { kIdle, kChecking, kDownloading, kWaiting, kInstalling, kFailed };
    // Why the last attempt failed: GitHub or the download could not be reached, the release has no apk-store.apk
    // GitHub gives a SHA-256 of, the file downloaded is not the release's, or its install failed.
    enum class Failure { kNone, kUnreached, kNoRelease, kUnverified, kInstall };

    // A check of the newest release starts. False, changing nothing, while one is under way (checking,
    // downloading, waiting to install or installing).
    bool begin_check() {
        if (busy()) return false;
        step_ = Step::kChecking;
        failure_ = Failure::kNone;
        percent_ = 0;
        return true;
    }

    // The release is back. `reached`: GitHub answered; `found`: its apk-store.apk can be checked
    // (store_asset_checkable); `same`: its SHA-256 is that of the store app installed; `installed`: there is
    // one. True when the download should start.
    bool checked(bool reached, bool found, bool same, bool installed) {
        if (step_ != Step::kChecking) return false;
        if (reached && found && !(same && installed)) {
            step_ = Step::kDownloading;
            percent_ = 0;
            return true;
        }
        if (installed) {
            step_ = Step::kIdle;  // up to date, or nothing to update to: the installed store stays
        } else {
            step_ = Step::kFailed;
            failure_ = !reached ? Failure::kUnreached : Failure::kNoRelease;
        }
        return false;
    }

    void progress(int percent) {
        if (step_ == Step::kDownloading) percent_ = percent;
    }

    // The download is back: `reached` it came whole, `verified` it is the release's file (judge_store_file).
    // `running`: the store app runs now. True when the install should start now.
    bool downloaded(bool reached, bool verified, bool running) {
        if (step_ != Step::kDownloading) return false;
        if (!reached || !verified) {
            step_ = Step::kFailed;
            failure_ = !reached ? Failure::kUnreached : Failure::kUnverified;
            return false;
        }
        step_ = running ? Step::kWaiting : Step::kInstalling;
        return !running;
    }

    // A checked download waits for the store app to stop; true when its install should start now.
    bool may_install(bool running) {
        if (step_ != Step::kWaiting || running) return false;
        step_ = Step::kInstalling;
        return true;
    }

    // The install ended.
    void installed(bool ok) {
        if (step_ != Step::kInstalling) return;
        step_ = ok ? Step::kIdle : Step::kFailed;
        failure_ = ok ? Failure::kNone : Failure::kInstall;
    }

    Step step() const { return step_; }
    Failure failure() const { return failure_; }
    int percent() const { return percent_; }
    bool busy() const { return step_ != Step::kIdle && step_ != Step::kFailed; }

private:
    Step step_ = Step::kIdle;
    Failure failure_ = Failure::kNone;
    int percent_ = 0;
};

}  // namespace winapk::launcher
