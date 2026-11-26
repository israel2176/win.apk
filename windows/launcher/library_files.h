// library_files.h -- the files the library keeps of an app, and how an install or an update changes them.
//
// An app is library\<name>.apk, and when it came as a bundle (.apkm, .xapk, .apks) library\<name>.splits
// beside it: hard links to the installed APKs (android\data\app\~~<nonce>\<package>-<nonce>\base.apk and
// split_*.apk, which com.winapk.tools.AppInstall writes), so each APK is on disk once. A plain APK was
// always one file (AppInstall links base.apk to it). A bundle is in the library only while it installs: a
// zip's members cannot be links, so keeping it kept every APK twice (a 191 MB Waze .xapk took 380 MB).
// What goes with it is what AppInstall does not install: the bundle's own manifest.json and icon, and the
// whole-app alternatives of a bundletool set; AppInstall installs every split of the set it picks, so
// nothing the app runs from is lost. Given library\<name>.apk with a .splits folder beside it, AppInstall
// links them all again, so a reinstall (a stale record) needs nothing else.
//
// A kept file is never written: it is the installed APK itself. An update renames the app's entries aside
// as <entry>.previous, puts the new file in their place, and deletes the .previous ones once the new copy
// is installed, or puts them back when it is not; a cut-off update is put back the same way at the next
// start (recover_interrupted_installs). The bundle becomes links last of all, so a start that finds a
// bundle with its install made from it does that step again (keep_installed), which is also how a library
// from before this layout moves to it. A removal takes the entries out first (<entry>.removed), so that one a
// running process holds stops it with nothing of the app gone, and deletes them last.
//
// std::filesystem only: no Windows call and no lock, so that test/launcher_state_test.cpp drives these on
// real files (launcher_main.cpp holds the app's InstallClaim around them).

#pragma once

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace winapk::launcher {

namespace lib_fs = std::filesystem;

inline std::wstring lower_ascii(std::wstring s) {
    for (wchar_t& c : s) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return s;
}

// Whether `p` is a split bundle (a zip of APKs) rather than an APK.
inline bool is_bundle_file(const lib_fs::path& p) {
    const std::wstring e = lower_ascii(p.extension().wstring());
    return e == L".apkm" || e == L".xapk" || e == L".apks";
}

// The library's entries of app `name` renamed with `suffix` (none: as they are): <name>.apk, a bundle
// <name>.apkm / .xapk / .apks, the <name>.splits folder, each followed by the suffix.
inline std::vector<lib_fs::path> app_entries_with(const lib_fs::path& library, const std::wstring& name,
                                                  const std::wstring& suffix) {
    static const wchar_t* const kKinds[] = {L".apk", L".apkm", L".xapk", L".apks", L".splits"};
    const std::wstring want = lower_ascii(name), tail = lower_ascii(suffix);
    std::vector<lib_fs::path> out;
    std::error_code ec;
    for (lib_fs::directory_iterator it(library, ec), end; !ec && it != end; it.increment(ec)) {
        std::wstring f = lower_ascii(it->path().filename().wstring());
        if (!tail.empty()) {
            if (f.size() <= tail.size() || f.compare(f.size() - tail.size(), tail.size(), tail)) continue;
            f.resize(f.size() - tail.size());
        }
        for (const wchar_t* k : kKinds) {
            if (f == want + k) {
                out.push_back(it->path());
                break;
            }
        }
    }
    return out;
}

// The library's entries of app `name`. With `previous`, those an update set aside instead (<entry>.previous).
inline std::vector<lib_fs::path> app_entries(const lib_fs::path& library, const std::wstring& name, bool previous) {
    return app_entries_with(library, name, previous ? L".previous" : L"");
}

// What a removal renames the app's entries to (take_out), which nothing reads as an app.
inline const wchar_t* const kRemovedSuffix = L".removed";

// The first step of removing app `name`: each of its entries renamed to <entry>.removed. Windows will not rename a
// file a process holds open without sharing its deletion (the app's runtime, still running), so this is the step
// that can fail, and it comes before anything else of the app goes: false then, with every entry renamed back, and
// the app whole in the library. Once it is true the removal goes on (drop_taken_out).
inline bool take_out(const lib_fs::path& library, const std::wstring& name) {
    std::error_code ec;
    std::vector<std::pair<lib_fs::path, lib_fs::path>> done;
    for (const lib_fs::path& p : app_entries(library, name, false)) {
        lib_fs::path out = p;
        out += kRemovedSuffix;
        lib_fs::remove_all(out, ec);  // a removal's that could not finish
        lib_fs::rename(p, out, ec);
        if (ec) {
            for (auto it = done.rbegin(); it != done.rend(); ++it) lib_fs::rename(it->second, it->first, ec);
            return false;
        }
        done.emplace_back(p, out);
    }
    return true;
}

// The removal's last step: the entries taken out deleted. True when none is left; one that could not be deleted is
// tried again at the next start (drop_all_taken_out), and is never an app meanwhile.
inline bool drop_taken_out(const lib_fs::path& library, const std::wstring& name) {
    std::error_code ec;
    bool all = true;
    for (const lib_fs::path& p : app_entries_with(library, name, kRemovedSuffix)) {
        lib_fs::remove_all(p, ec);
        all = all && !ec;
    }
    return all;
}

// At a start: whatever a removal could not delete before (<entry>.removed of any app).
inline void drop_all_taken_out(const lib_fs::path& library) {
    std::error_code ec;
    std::vector<lib_fs::path> left;
    for (lib_fs::directory_iterator it(library, ec), end; !ec && it != end; it.increment(ec)) {
        if (lower_ascii(it->path().extension().wstring()) == kRemovedSuffix) left.push_back(it->path());
    }
    for (const lib_fs::path& p : left) lib_fs::remove_all(p, ec);
}

// Before an install of `name` from another file: each entry of the app's copy renamed to <entry>.previous.
// False when one could not be (the copy is then as it was: put_back): the install must not go on, since
// writing over a kept file would write into the installed app.
inline bool set_aside(const lib_fs::path& library, const std::wstring& name) {
    std::error_code ec;
    for (const lib_fs::path& p : app_entries(library, name, false)) {
        lib_fs::path kept = p;
        kept += L".previous";
        lib_fs::remove_all(kept, ec);
        lib_fs::rename(p, kept, ec);
        if (ec) return false;
    }
    return true;
}

// After an install that did not go through, or was cut off: what replaced the app's entries removed, and the
// entries set aside put back. False when nothing had been set aside (a new app).
inline bool put_back(const lib_fs::path& library, const std::wstring& name) {
    const std::vector<lib_fs::path> kept = app_entries(library, name, true);
    if (kept.empty()) return false;
    std::error_code ec;
    for (const lib_fs::path& p : app_entries(library, name, false)) lib_fs::remove_all(p, ec);
    for (const lib_fs::path& k : kept) {
        lib_fs::path p = k;
        p.replace_extension();  // <entry>.previous -> <entry>
        lib_fs::rename(k, p, ec);
    }
    return true;
}

// After an install that went through: the entries set aside go.
inline void drop_previous(const lib_fs::path& library, const std::wstring& name) {
    std::error_code ec;
    for (const lib_fs::path& k : app_entries(library, name, true)) lib_fs::remove_all(k, ec);
}

// Whether AppInstall's stamp in an installed copy (<copy>\.installed: "<APKs> <bytes>:<mtime ms>\n") says it
// was made from a file of this size and modification time (milliseconds since 1970).
inline bool stamp_matches(const std::string& stamp, uintmax_t size, long long mtime_ms) {
    const size_t space = stamp.find(' ');
    const size_t colon = stamp.find(':', space == std::string::npos ? 0 : space);
    if (space == std::string::npos || colon == std::string::npos) return false;
    const std::string bytes = stamp.substr(space + 1, colon - space - 1);
    const std::string when = stamp.substr(colon + 1);
    char* end = nullptr;
    const unsigned long long b = std::strtoull(bytes.c_str(), &end, 10);
    if (bytes.empty() || *end) return false;
    const long long t = std::strtoll(when.c_str(), &end, 10);
    if (when.empty() || (*end && *end != '\r' && *end != '\n')) return false;
    return b == size && t == mtime_ms;
}

// The installed copy's APKs other than base.apk, as AppInstall names them (split_*.apk).
inline std::vector<lib_fs::path> installed_splits(const lib_fs::path& dir) {
    std::vector<lib_fs::path> out;
    std::error_code ec;
    for (lib_fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring f = it->path().filename().wstring();
        if (f.size() > 10 && f.compare(0, 6, L"split_") == 0 && lower_ascii(f.substr(f.size() - 4)) == L".apk" &&
            it->is_regular_file(ec)) {
            out.push_back(it->path());
        }
    }
    return out;
}

// `bundle` (library\<name>.<ext>), installed as `dir`, replaced by library\<name>.apk and library\<name>.splits\:
// hard links to dir's base.apk and split_*.apk. Returns the .apk, or empty when the links could not be made (a
// volume without hard links): the bundle then stays, as before this layout. Whatever an earlier attempt left
// is redone, in an order that leaves the bundle until last: a start that finds it does this again.
inline lib_fs::path keep_installed(const lib_fs::path& bundle, const lib_fs::path& dir) {
    std::error_code ec;
    const lib_fs::path library = bundle.parent_path();
    const std::wstring name = bundle.stem().wstring();
    const lib_fs::path apk = library / (name + L".apk"), splits = library / (name + L".splits");
    const lib_fs::path apk_new = library / (name + L".apk.new"), splits_new = library / (name + L".splits.new");
    if (!lib_fs::is_regular_file(dir / L"base.apk", ec)) return {};
    lib_fs::remove_all(apk_new, ec);
    lib_fs::remove_all(splits_new, ec);
    const std::vector<lib_fs::path> parts = installed_splits(dir);
    bool ok = (lib_fs::create_hard_link(dir / L"base.apk", apk_new, ec), !ec);
    if (ok && !parts.empty()) ok = lib_fs::create_directory(splits_new, ec) && !ec;
    for (size_t i = 0; ok && i < parts.size(); ++i) {
        lib_fs::create_hard_link(parts[i], splits_new / parts[i].filename(), ec);
        ok = !ec;
    }
    if (!ok) {
        lib_fs::remove_all(apk_new, ec);
        lib_fs::remove_all(splits_new, ec);
        return {};
    }
    lib_fs::remove_all(splits, ec);
    if (!parts.empty()) {
        lib_fs::rename(splits_new, splits, ec);
        if (ec) return {};
    }
    lib_fs::remove(apk, ec);
    lib_fs::rename(apk_new, apk, ec);
    if (ec) return {};
    lib_fs::remove(bundle, ec);
    return apk;
}

}  // namespace winapk::launcher
