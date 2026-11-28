// user_verification_test.cpp -- winapk-user-verification-test: what Windows Hello's answers mean to
// Android's BiometricManager and BiometricPrompt (host/user_verification.h), which makes no Windows call.
//
//   winapk-user-verification-test
//
// canAuthenticate for each authenticator with and without Hello and a sensor; each of Hello's answers to
// a prompt (its Cancel as the negative button only where Android has one); a key's validity window.

#include <cstdio>

#include "user_verification.h"

namespace {

using namespace winapk::host::user_verification;

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

const Host kPinOnly{Availability::kAvailable, 0};
const Host kFingerprint{Availability::kAvailable, 1};
const Host kNoHello{Availability::kNotConfiguredForUser, 1};
const Host kNoDevice{Availability::kDeviceNotPresent, 0};
const Host kPolicy{Availability::kDisabledByPolicy, 1};

}  // namespace

int main() {
    std::printf("canAuthenticate\n");
    check("a fingerprint reader: BIOMETRIC_WEAK", can_authenticate(kWeak, kFingerprint) == kSuccess);
    check("a fingerprint reader: BIOMETRIC_STRONG", can_authenticate(kStrong, kFingerprint) == kSuccess);
    check("a fingerprint reader: DEVICE_CREDENTIAL", can_authenticate(kCredential, kFingerprint) == kSuccess);
    check("the PIN alone: no biometric hardware", can_authenticate(kWeak, kPinOnly) == kNoHardware);
    check("the PIN alone: DEVICE_CREDENTIAL", can_authenticate(kCredential, kPinOnly) == kSuccess);
    check("the PIN alone: BIOMETRIC_WEAK | DEVICE_CREDENTIAL",
          can_authenticate(kWeak | kCredential, kPinOnly) == kSuccess);
    check("Hello not set up: none enrolled", can_authenticate(kWeak, kNoHello) == kNoneEnrolled);
    check("Hello not set up: no credential either", can_authenticate(kCredential, kNoHello) == kNoneEnrolled);
    check("no Hello device: no hardware", can_authenticate(kCredential, kNoDevice) == kNoHardware);
    check("Hello off by policy: unavailable", can_authenticate(kWeak, kPolicy) == kHwUnavailable);
    check("Windows not asked: no hardware", can_authenticate(kWeak, Host{}) == kNoHardware);
    check("no authenticator: unsupported", can_authenticate(0, kFingerprint) == kUnsupported);
    check("an unknown bit: unsupported", can_authenticate(0x10000, kFingerprint) == kUnsupported);

    std::printf("a prompt's end\n");
    Outcome o = outcome(Verification::kVerified, kWeak, kFingerprint, true);
    check("verified, biometric asked: biometric", o.error == kSuccess && o.type == kTypeBiometric);
    check("verified, biometric asked: grants the biometric only", o.granted == kStrong);
    o = outcome(Verification::kVerified, kCredential, kFingerprint, false);
    check("verified, credential asked: credential", o.error == kSuccess && o.type == kTypeCredential &&
                                                        o.granted == kCredential);
    o = outcome(Verification::kVerified, kWeak | kCredential, kPinOnly, false);
    check("verified with the PIN alone: credential", o.error == kSuccess && o.type == kTypeCredential &&
                                                         o.granted == kCredential);
    o = outcome(Verification::kVerified, kStrong | kCredential, kFingerprint, false);
    check("verified, both asked: both granted", o.granted == (kStrong | kCredential));
    check("Cancel with a negative button: the negative button",
          outcome(Verification::kCanceled, kWeak, kFingerprint, true).error == kNegativeButton);
    check("Cancel without one: user canceled",
          outcome(Verification::kCanceled, kWeak, kFingerprint, false).error == kUserCanceled);
    check("Cancel where the credential is allowed: user canceled",
          outcome(Verification::kCanceled, kWeak | kCredential, kFingerprint, true).error == kUserCanceled);
    check("retries exhausted: lockout", outcome(Verification::kRetriesExhausted, kWeak, kFingerprint, true).error ==
                                            kLockout);
    check("device busy: unavailable", outcome(Verification::kDeviceBusy, kWeak, kFingerprint, true).error ==
                                          kHwUnavailable);
    check("a failed request: unable to process, nothing granted",
          outcome(Verification::kFailed, kWeak, kFingerprint, true).error == kUnableToProcess &&
              outcome(Verification::kFailed, kWeak, kFingerprint, true).granted == 0);

    std::printf("a key's validity window\n");
    check("biometric 5 s ago, 10 s key", within(kStrong, 95000, 0, 100000, 10));
    check("biometric 15 s ago, 10 s key", !within(kStrong, 85000, 0, 100000, 10));
    check("credential only, biometric key", !within(kStrong, 0, 99000, 100000, 10));
    check("credential, key of either", within(kStrong | kCredential, 0, 99000, 100000, 10));
    check("never verified", !within(kStrong | kCredential, 0, 0, 100000, 10));
    check("a per-use key has no window", !within(kStrong, 99999, 0, 100000, 0));

    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
