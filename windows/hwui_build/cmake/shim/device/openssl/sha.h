// SHA-256 for ShaderCache's validation hash, on Windows' CNG.
#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <stddef.h>
#include <stdint.h>
#define SHA256_DIGEST_LENGTH 32
static inline uint8_t* SHA256(const uint8_t* data, size_t len, uint8_t* out) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0) {
        if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
            BCryptHashData(h, (PUCHAR)data, (ULONG)len, 0);
            BCryptFinishHash(h, out, SHA256_DIGEST_LENGTH, 0);
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return out;
}

// The streaming form (ShaderCache's identity hash).
typedef struct {
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash;
} SHA256_CTX;
static inline int SHA256_Init(SHA256_CTX* c) {
    c->alg = NULL;
    c->hash = NULL;
    if (BCryptOpenAlgorithmProvider(&c->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return 0;
    return BCryptCreateHash(c->alg, &c->hash, NULL, 0, NULL, 0, 0) == 0;
}
static inline int SHA256_Update(SHA256_CTX* c, const void* data, size_t len) {
    return c->hash && BCryptHashData(c->hash, (PUCHAR)data, (ULONG)len, 0) == 0;
}
static inline int SHA256_Final(uint8_t* out, SHA256_CTX* c) {
    int ok = c->hash && BCryptFinishHash(c->hash, out, SHA256_DIGEST_LENGTH, 0) == 0;
    if (c->hash) BCryptDestroyHash(c->hash);
    if (c->alg) BCryptCloseAlgorithmProvider(c->alg, 0);
    return ok;
}
