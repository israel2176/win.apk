// user_verification.h -- what Windows Hello's answers mean to Android's BiometricManager and
// BiometricPrompt, with no Windows call (test/user_verification_test.cpp). The calls themselves are
// native_user_verification.cpp's.
//
// Windows Hello (Windows.Security.Credentials.UI.UserConsentVerifier) checks that the PC's user is
// there, by a fingerprint, a face or the Hello PIN, whichever the user has set up and picks in its
// dialog. Android's authenticators map onto it as a phone's do onto its own:
//   - DEVICE_CREDENTIAL (the screen lock) is the Hello PIN: there whenever Hello is set up.
//   - BIOMETRIC_STRONG and BIOMETRIC_WEAK are a fingerprint reader or a face camera Windows Biometric
//     Framework lists: a PC with none answers BIOMETRIC_ERROR_NO_HARDWARE, as a phone without a
//     sensor does.
// Where it differs from a phone: Hello's dialog always offers the PIN beside a biometric, so a prompt
// for biometrics alone can be passed with the PIN, and its answer does not say which was used (a
// success is reported as biometric whenever a biometric was asked for and a sensor exists).

#pragma once

#include <cstdint>

namespace winapk::host::user_verification {

// BiometricManager.Authenticators.
constexpr int kStrong = 0x000f, kWeak = 0x00ff, kCredential = 0x8000;
constexpr int kBiometricBits = kWeak;

// BiometricManager / BiometricPrompt codes.
constexpr int kSuccess = 0;
constexpr int kHwUnavailable = 1;
constexpr int kUnableToProcess = 2;
constexpr int kLockout = 7;
constexpr int kUserCanceled = 10;
constexpr int kNoneEnrolled = 11;
constexpr int kNoHardware = 12;
constexpr int kNegativeButton = 13;
constexpr int kUnsupported = -2;

// BiometricPrompt.AUTHENTICATION_RESULT_TYPE_*.
constexpr int kTypeCredential = 1, kTypeBiometric = 2;

// UserConsentVerifierAvailability; kUnknown when Windows could not be asked.
enum class Availability : int {
    kAvailable = 0,
    kDeviceNotPresent = 1,
    kNotConfiguredForUser = 2,
    kDisabledByPolicy = 3,
    kDeviceBusy = 4,
    kUnknown = -1,
};

// UserConsentVerificationResult.
enum class Verification : int {
    kVerified = 0,
    kDeviceNotPresent = 1,
    kNotConfiguredForUser = 2,
    kDisabledByPolicy = 3,
    kDeviceBusy = 4,
    kRetriesExhausted = 5,
    kCanceled = 6,
    kFailed = -1,  // the request itself failed (no Hello, an error HRESULT)
};

// What the PC has: Hello's state, and how many biometric sensors Windows lists.
struct Host {
    Availability hello = Availability::kUnknown;
    int sensors = 0;
};

// Whether `mask` names only authenticators Android has.
inline bool valid_mask(int mask) { return mask != 0 && (mask & ~(kBiometricBits | kCredential)) == 0; }

// The authenticators of `mask` this PC can verify: kStrong for the biometrics, kCredential for the
// PIN; 0 for none.
inline int usable(int mask, const Host& host) {
    if (host.hello != Availability::kAvailable) return 0;
    int out = 0;
    if ((mask & kBiometricBits) != 0 && host.sensors > 0) out |= kStrong;
    if ((mask & kCredential) != 0) out |= kCredential;
    return out;
}

// BiometricManager.canAuthenticate(mask).
inline int can_authenticate(int mask, const Host& host) {
    if (!valid_mask(mask)) return kUnsupported;
    if (usable(mask, host) != 0) return kSuccess;
    switch (host.hello) {
        case Availability::kNotConfiguredForUser: return kNoneEnrolled;
        case Availability::kDisabledByPolicy:
        case Availability::kDeviceBusy: return kHwUnavailable;
        case Availability::kAvailable:  // only biometrics asked for, and no sensor
        case Availability::kDeviceNotPresent:
        case Availability::kUnknown: break;
    }
    return kNoHardware;
}

// A finished prompt, as BiometricPrompt reports it: error kSuccess with the result type, or the error.
struct Outcome {
    int error = kUnableToProcess;
    int type = 0;
    int granted = 0;  // usable()'s bits a success vouches for: what keys and getLastAuthenticationTime see
};

// Hello's answer to a prompt for `mask`. `negative_button`: the prompt has one (setNegativeButton), so
// Hello's Cancel is that button, as there is no other way out of Hello's dialog.
inline Outcome outcome(Verification result, int mask, const Host& host, bool negative_button) {
    Outcome o;
    switch (result) {
        case Verification::kVerified: {
            const int granted = usable(mask, Host{Availability::kAvailable, host.sensors});
            if (granted == 0) return o;  // asked for nothing this PC has: no success to report
            o.error = kSuccess;
            o.granted = granted;
            o.type = (granted & kStrong) != 0 ? kTypeBiometric : kTypeCredential;
            return o;
        }
        case Verification::kDeviceNotPresent: o.error = kNoHardware; return o;
        case Verification::kNotConfiguredForUser: o.error = kNoneEnrolled; return o;
        case Verification::kDisabledByPolicy:
        case Verification::kDeviceBusy: o.error = kHwUnavailable; return o;
        case Verification::kRetriesExhausted: o.error = kLockout; return o;
        case Verification::kCanceled:
            // A prompt that allows the device credential has no negative button on Android.
            o.error = negative_button && (mask & kCredential) == 0 ? kNegativeButton : kUserCanceled;
            return o;
        case Verification::kFailed: break;
    }
    return o;
}

// Whether a key whose use needs `key_mask` (kStrong, kCredential or both) may be used, given when each
// was last verified (milliseconds on one monotonic clock, 0 never) and now.
inline bool within(int key_mask, uint64_t last_biometric, uint64_t last_credential, uint64_t now, int seconds) {
    if (seconds <= 0) return false;
    const uint64_t window = static_cast<uint64_t>(seconds) * 1000;
    auto fresh = [&](uint64_t t) { return t != 0 && now >= t && now - t <= window; };
    return ((key_mask & kStrong) != 0 && fresh(last_biometric)) ||
           ((key_mask & kCredential) != 0 && fresh(last_credential));
}

}  // namespace winapk::host::user_verification
