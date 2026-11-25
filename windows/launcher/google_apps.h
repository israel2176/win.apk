// google_apps.h -- the versions of Google's three apps the Google services setup installs.
//
// Download asks APKPure (apkeep, windows/scripts/fetch_apkeep.sh) for exactly these versions, as
// `apkeep -a <package>@<version>`, never for the newest: what APKPure lists newest has been a preview build
// (Services Framework's, minSdkVersion "Q", which a phone's installer refuses on a release build) and an x86-only
// variant (Play services'). Each was chosen for Win.apk, which presents Android 14 (API level 34):
//
//   Services Framework  14-10182995, the build for Android 14 (minSdkVersion 34), no native code.
//   Play services       26.46.32 (040800-...), the x86 + x86_64 nodpi variant for Android 7.0+ (minSdkVersion 24).
//   Play Store          54.2.12-24 [0] [PR] ..., the universal nodpi variant for Android 7.0+ (minSdkVersion 24).
//
// They set only the first install: from then on the Play Store updates itself and Play services, as on a phone.
// Every file is still checked by `winapk-runtime --verify-apk` before it is installed (Google's signature,
// x86_64 code when it has any, a minSdkVersion Win.apk presents).
//
// CI's "Google's pinned apps" step (.github/workflows/ci.yml) reads the lines below, one `{"<package>",
// "<version>"},` each, downloads each version with the staged apkeep as the launcher does and checks it with
// --verify-apk: keep that form. It runs when this file, fetch_apkeep.sh or apkeep's patches change.

#pragma once

struct GooglePin {
    const char* pkg;
    const char* version;  // APKPure's version name, which is the manifest's versionName
};

constexpr GooglePin kGooglePins[] = {
    {"com.google.android.gsf", "14-10182995"},
    {"com.google.android.gms", "26.46.32 (040800-1025220680)"},
    {"com.android.vending", "54.2.12-24 [0] [PR] 1019424474"},
};
