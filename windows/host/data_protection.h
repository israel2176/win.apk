// data_protection.h -- secrets at rest under Windows' data protection (DPAPI), for the Google
// account's tokens (com.macapk.gms.GmsStore's gms\tokens.dat) and the system broker's accounts
// table (com.macapk.accounts.AccountStore's accounts_ce.dat), which hold the account's master
// token. Java reaches it through com.macapk.loader.DataProtection (native_data_protection.cpp).
//
// CryptProtectData in the current user's scope, as the launcher seals the app-ops signing key
// (appops\signing-key.p256), plus a fixed entropy so a blob is opened only by a caller that
// names it: another program of the same user calling CryptUnprotectData on the file alone gets
// nothing. What this is not: a barrier against code running as the same user that knows the
// entropy (every app's native code runs as the user and could read the constant out of this
// binary). It keeps the token out of plain files, backups and copies of the data folder taken to
// another account or PC; keeping it from the apps is done by which processes read it at all
// (docs/ARCHITECTURE.md, "Google services").
//
// Header-only and free of anything but Windows' crypt32, so test/data_protection_test.cpp
// checks it without the runtime.
#pragma once

#include <string>

#include <windows.h>
#include <dpapi.h>

namespace winapk::host::data_protection {

// Part of every blob's key: a blob sealed with other entropy (or none) does not open, and one of
// these does not open without it. Changing it makes every sealed file unreadable (the user signs
// in again), so it carries a version.
inline constexpr char kEntropy[] = "Win.apk protected data v1";

inline DATA_BLOB entropy_blob() {
    return DATA_BLOB{static_cast<DWORD>(sizeof kEntropy - 1),
                     reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
}

// `plain` sealed for this user: true with the blob in `*out`, false when DPAPI refuses.
inline bool seal(const std::string& plain, std::string* out) {
    DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB extra = entropy_blob();
    DATA_BLOB sealed{};
    if (!CryptProtectData(&in, L"Win.apk protected data", &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                          &sealed))
        return false;
    out->assign(reinterpret_cast<const char*>(sealed.pbData), sealed.cbData);
    LocalFree(sealed.pbData);
    return true;
}

// A blob seal() made, opened: false when it is not one, was changed, or is another user's (or
// another PC's) -- DPAPI checks its own integrity, so a changed byte never opens as other data.
inline bool unseal(const std::string& blob, std::string* out) {
    DATA_BLOB in{static_cast<DWORD>(blob.size()), reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()))};
    DATA_BLOB extra = entropy_blob();
    DATA_BLOB plain{};
    if (!CryptUnprotectData(&in, nullptr, &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) return false;
    out->assign(reinterpret_cast<const char*>(plain.pbData), plain.cbData);
    SecureZeroMemory(plain.pbData, plain.cbData);
    LocalFree(plain.pbData);
    return true;
}

}  // namespace winapk::host::data_protection
