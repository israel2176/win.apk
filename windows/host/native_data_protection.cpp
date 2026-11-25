// native_data_protection.cpp -- com.macapk.loader.DataProtection on Windows: seal and open a
// secret with DPAPI (data_protection.h). Null for a refusal; the Java side says what failed.

#include "host_natives.h"
#include "data_protection.h"

#include <string>

namespace winapk::host {

namespace {

std::string bytes_of(JNIEnv* env, jbyteArray a) {
    std::string s;
    if (!a) return s;
    const jsize n = env->GetArrayLength(a);
    s.resize(static_cast<size_t>(n));
    if (n > 0) env->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte*>(s.data()));
    return s;
}

jbyteArray array_of(JNIEnv* env, std::string* s) {
    jbyteArray out = env->NewByteArray(static_cast<jsize>(s->size()));
    if (out && !s->empty())
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(s->size()), reinterpret_cast<const jbyte*>(s->data()));
    SecureZeroMemory(s->data(), s->size());
    return out;
}

jbyteArray JNICALL DataProtection_seal(JNIEnv* env, jclass, jbyteArray jplain) {
    std::string plain = bytes_of(env, jplain), sealed;
    const bool ok = data_protection::seal(plain, &sealed);
    SecureZeroMemory(plain.data(), plain.size());
    return ok ? array_of(env, &sealed) : nullptr;
}

jbyteArray JNICALL DataProtection_unseal(JNIEnv* env, jclass, jbyteArray jsealed) {
    std::string plain;
    if (!data_protection::unseal(bytes_of(env, jsealed), &plain)) return nullptr;
    return array_of(env, &plain);
}

const HostMethod kMethods[] = {
    WINAPK_HOST_NATIVE("com.macapk.loader.DataProtection", "nativeSeal", "([B)[B", DataProtection_seal),
    WINAPK_HOST_NATIVE("com.macapk.loader.DataProtection", "nativeUnseal", "([B)[B", DataProtection_unseal),
};

}  // namespace

HostMethods native_data_protection_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

}  // namespace winapk::host
