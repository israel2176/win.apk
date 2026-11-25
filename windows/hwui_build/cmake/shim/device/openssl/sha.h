// SHA-256 for ShaderCache's validation hash, on Windows' CNG.
#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <stddef.h>
#include <stdint.h>
#define SHA256_DIGEST_LENGTH 32
static inline uint8_t* SHA256(const uint8_t* data, size_t len, uint8_t* out) {
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)data, (ULONG)len, out, SHA256_DIGEST_LENGTH);
    return out;
}
