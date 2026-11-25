// data_protection_test.cpp -- winapk-data-protection-test: DPAPI sealing of the Google account's
// tokens at rest (host/data_protection.h). Runs under Wine too.
//
//   winapk-data-protection-test

#include <cstdio>
#include <cstring>
#include <string>

#include "data_protection.h"

namespace {

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

namespace dp = winapk::host::data_protection;

}  // namespace

int main() {
    std::printf("sealing\n");
    const std::string secret = "account.someone@example.com.masterToken=aas_et/NOT-A-REAL-TOKEN==\n";
    std::string sealed;
    check("a secret seals", dp::seal(secret, &sealed));
    check("the blob does not carry the secret in the clear", sealed.find("NOT-A-REAL-TOKEN") == std::string::npos);
    std::string opened;
    check("the blob opens", dp::unseal(sealed, &opened));
    check("to the same bytes", opened == secret);

    std::string empty_sealed, empty_opened = "x";
    check("an empty secret seals", dp::seal("", &empty_sealed));
    check("and opens empty", dp::unseal(empty_sealed, &empty_opened) && empty_opened.empty());

    std::string binary(256, '\0');
    for (int i = 0; i < 256; ++i) binary[i] = static_cast<char>(i);
    std::string bsealed, bopened;
    check("bytes of every value round-trip", dp::seal(binary, &bsealed) && dp::unseal(bsealed, &bopened) &&
                                                 bopened == binary);

    std::printf("refusals\n");
    // DPAPI signs the blob: a changed byte either refuses it or (in a field the signature leaves
    // out, such as the description) still opens the same secret, never another.
    std::string out;
    int other = 0, refused = 0;
    for (size_t i = 0; i < sealed.size(); ++i) {
        std::string changed = sealed;
        changed[i] = static_cast<char>(changed[i] ^ 0x5a);
        if (!dp::unseal(changed, &out)) ++refused;
        else if (out != secret) ++other;
    }
    check("no changed byte opens as another secret", other == 0);
    check("a changed byte is refused in most of the blob", refused * 2 > static_cast<int>(sealed.size()));
    std::string last = sealed;
    last.back() = static_cast<char>(last.back() ^ 0x01);
    check("a changed signature does not open", !dp::unseal(last, &out));
    check("a truncated blob does not open", !dp::unseal(sealed.substr(0, sealed.size() - 7), &out));
    check("plain text is not a blob", !dp::unseal(secret, &out));
    check("nothing is not a blob", !dp::unseal("", &out));

    // The entropy is part of the key: a blob sealed with none (what any program of the user could
    // open with CryptUnprotectData alone) is not ours, and ours does not open without it.
    DATA_BLOB in{static_cast<DWORD>(secret.size()), reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()))};
    DATA_BLOB bare{};
    const bool bare_ok =
        CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &bare) != FALSE;
    check("(a blob sealed with no entropy)", bare_ok);
    if (bare_ok) {
        check("a blob sealed with no entropy does not open as ours",
              !dp::unseal(std::string(reinterpret_cast<const char*>(bare.pbData), bare.cbData), &out));
        LocalFree(bare.pbData);
    }
    DATA_BLOB ours{static_cast<DWORD>(sealed.size()), reinterpret_cast<BYTE*>(sealed.data())};
    DATA_BLOB plain{};
    const bool opened_bare =
        CryptUnprotectData(&ours, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain) != FALSE;
    if (opened_bare) LocalFree(plain.pbData);
    check("ours does not open without the entropy", !opened_bare);

    std::printf(g_failures ? "%d FAILED\n" : "ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
