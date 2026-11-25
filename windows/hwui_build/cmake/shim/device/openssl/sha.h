// SHA-256 for ShaderCache's validation hash, on Windows' CNG.
#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <stddef.h>
#include <stdint.h>
#define SHA256_DIGEST_LENGTH 32
static inline uint8_t* SHA256(const uint8_t* data, size_t len, uint8_t* out) {
    BCRYPT_ALG_HANDLE alg = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0) {
        BCryptHash(alg, NULL, 0, (PUCHAR)data, (ULONG)len, out, SHA256_DIGEST_LENGTH);
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return out;
}
