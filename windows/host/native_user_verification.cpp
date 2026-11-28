// native_user_verification.cpp -- com.macapk.loader.UserVerificationBridge on Windows: Android's
// BiometricManager, BiometricPrompt, KeyguardManager's confirm-credential screen and the Keystore's
// keys that need the user (setUserAuthenticationRequired) on Windows Hello. user_verification.h says
// what each answer means; this file asks Windows.
//
// WinRT is used through its ABI, declared here (MinGW's headers lack Windows.Security.Credentials.UI),
// with combase loaded at run time. Each question runs on a thread of its own in the multithreaded
// apartment: a check of what the PC has is waited for (cached for a few seconds), a prompt is not:
// its thread posts the end to UserVerificationBridge.onNativeCompleted, which takes the result.
//
// What a success authorizes stays here, never in Java: the Keystore operation (the CipherSpi, MacSpi
// or SignatureSpi behind the prompt's CryptoObject) a prompt was given, and when the user was last
// verified, for keys with a validity time.

#include "host_natives.h"
#include "user_verification.h"
#include "win32_window.h"

#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <inspectable.h>
#include <unknwn.h>

namespace winapk::host {

namespace {

namespace uv = user_verification;

// ---- the WinRT ABI we use ------------------------------------------------------

struct IAsyncInfoAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int* status) = 0;  // 0 started, 1 completed, 2 canceled, 3 error
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* error) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

// IAsyncOperation<UserConsentVerifierAvailability> and <UserConsentVerificationResult>: both enums,
// so one shape serves both.
struct IAsyncEnum : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(int* results) = 0;
};

// Windows.Security.Credentials.UI.IUserConsentVerifierStatics.
struct IUserConsentVerifierStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CheckAvailabilityAsync(IAsyncEnum** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE RequestVerificationAsync(HSTRING message, IAsyncEnum** result) = 0;
};

// userconsentverifierinterop.h: the dialog owned by a desktop program's window.
struct IUserConsentVerifierInterop : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RequestVerificationForWindowAsync(HWND window, HSTRING message, REFIID riid,
                                                                        void** operation) = 0;
};

constexpr GUID kIidStatics = {0xaf4f3f91, 0x564c, 0x4ddc, {0xb8, 0xb5, 0x97, 0x34, 0x47, 0x62, 0x7c, 0x65}};
constexpr GUID kIidInterop = {0x39e050c3, 0x4e74, 0x441a, {0x8d, 0xc0, 0xb8, 0x11, 0x04, 0xdf, 0x94, 0x9c}};
constexpr GUID kIidAsyncInfo = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
// IAsyncOperation<UserConsentVerificationResult>: a parameterized IID, derived from its signature as the
// WinRT type system does (SHA-1 over the pinterface namespace and
// "pinterface({9fc2b0bb-...};enum(Windows.Security.Credentials.UI.UserConsentVerificationResult;i4))").
constexpr GUID kIidAsyncResult = {0xfd596ffd, 0x2318, 0x558f, {0x9d, 0xbe, 0xd2, 0x1d, 0xf4, 0x37, 0x64, 0xa5}};

// ---- combase and winbio, loaded at run time ------------------------------------------

template <typename F>
F proc(HMODULE m, const char* name) {
    return m ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}

struct Combase {
    HRESULT(WINAPI* ro_initialize)(int) = nullptr;
    void(WINAPI* ro_uninitialize)() = nullptr;
    HRESULT(WINAPI* activation_factory)(HSTRING, REFIID, void**) = nullptr;
    HRESULT(WINAPI* create_string)(const wchar_t*, UINT32, HSTRING*) = nullptr;
    HRESULT(WINAPI* delete_string)(HSTRING) = nullptr;
    bool ok() const { return ro_initialize && ro_uninitialize && activation_factory && create_string && delete_string; }
};

const Combase& combase() {
    static const Combase api = [] {
        Combase c;
        HMODULE m = LoadLibraryW(L"combase.dll");
        c.ro_initialize = proc<decltype(c.ro_initialize)>(m, "RoInitialize");
        c.ro_uninitialize = proc<decltype(c.ro_uninitialize)>(m, "RoUninitialize");
        c.activation_factory = proc<decltype(c.activation_factory)>(m, "RoGetActivationFactory");
        c.create_string = proc<decltype(c.create_string)>(m, "WindowsCreateString");
        c.delete_string = proc<decltype(c.delete_string)>(m, "WindowsDeleteString");
        return c;
    }();
    return api;
}

// The number of fingerprint readers, face cameras and iris scanners Windows Biometric Framework lists.
int biometric_sensors() {
    using Enum = HRESULT(WINAPI*)(ULONG factor, void** units, SIZE_T* count);
    using Free = HRESULT(WINAPI*)(void* address);
    static HMODULE m = LoadLibraryW(L"winbio.dll");
    static const Enum enumerate = proc<Enum>(m, "WinBioEnumBiometricUnits");
    static const Free release = proc<Free>(m, "WinBioFree");
    if (!enumerate || !release) return 0;
    constexpr ULONG kFace = 0x02, kFingerprint = 0x08, kIris = 0x10;  // WINBIO_TYPE_*
    int total = 0;
    for (ULONG factor : {kFingerprint, kFace, kIris}) {
        void* units = nullptr;
        SIZE_T count = 0;
        if (SUCCEEDED(enumerate(factor, &units, &count))) total += static_cast<int>(count);
        if (units) release(units);
    }
    return total;
}

// One thread's WinRT: the MTA for as long as it lives.
class Apartment {
public:
    Apartment() : ok_(combase().ok() && SUCCEEDED(combase().ro_initialize(1 /* RO_INIT_MULTITHREADED */))) {}
    ~Apartment() {
        if (ok_) combase().ro_uninitialize();
    }
    bool ok() const { return ok_; }

private:
    bool ok_;
};

HSTRING make_string(const std::wstring& s) {
    HSTRING h = nullptr;
    if (FAILED(combase().create_string(s.c_str(), static_cast<UINT32>(s.size()), &h))) return nullptr;
    return h;
}

template <typename T>
T* factory(REFIID iid) {
    HSTRING name = make_string(L"Windows.Security.Credentials.UI.UserConsentVerifier");
    if (!name) return nullptr;
    T* out = nullptr;
    if (FAILED(combase().activation_factory(name, iid, reinterpret_cast<void**>(&out)))) out = nullptr;
    combase().delete_string(name);
    return out;
}

// Waits for `op`, calling Cancel once `cancel` is set; its enum, or -1 when it did not complete.
int wait_for(IAsyncEnum* op, const std::atomic<bool>* cancel, DWORD limit_ms) {
    IAsyncInfoAbi* info = nullptr;
    if (FAILED(op->QueryInterface(kIidAsyncInfo, reinterpret_cast<void**>(&info))) || !info) return -1;
    int status = 0;
    bool cancel_sent = false;
    const ULONGLONG deadline = GetTickCount64() + limit_ms;
    while (SUCCEEDED(info->get_Status(&status)) && status == 0) {
        if (!cancel_sent && ((cancel && cancel->load()) || GetTickCount64() > deadline)) {
            info->Cancel();
            cancel_sent = true;
        }
        Sleep(15);
    }
    int value = -1;
    if (status == 1 && FAILED(op->GetResults(&value))) value = -1;
    info->Close();
    info->Release();
    return value;
}

// ---- what the PC has, cached ----------------------------------------------------------------

std::mutex g_host_lock;
uv::Host g_host;
ULONGLONG g_host_at = 0;

uv::Host ask_host() {
    uv::Host h;
    Apartment apartment;
    if (!apartment.ok()) return h;
    if (IUserConsentVerifierStatics* s = factory<IUserConsentVerifierStatics>(kIidStatics)) {
        IAsyncEnum* op = nullptr;
        if (SUCCEEDED(s->CheckAvailabilityAsync(&op)) && op) {
            const int v = wait_for(op, nullptr, 5000);
            if (v >= 0 && v <= 4) h.hello = static_cast<uv::Availability>(v);
            op->Release();
        }
        s->Release();
    }
    h.sensors = biometric_sensors();
    return h;
}

// Asked on a thread of its own (the caller's apartment is the app's), at most every three seconds:
// BiometricManager.Strings asks for each of its labels.
uv::Host host_now() {
    std::lock_guard<std::mutex> l(g_host_lock);
    const ULONGLONG now = GetTickCount64();
    if (g_host_at == 0 || now - g_host_at > 3000) {
        uv::Host h;
        std::thread([&h] { h = ask_host(); }).join();
        g_host = h;
        g_host_at = now;
        std::fprintf(stderr, "[win.apk] user verification: Windows Hello %d, %d biometric sensor(s)\n",
                     static_cast<int>(h.hello), h.sensors);
    }
    return g_host;
}

// ---- prompts and what they authorized ------------------------------------------------------

JavaVM* g_vm = nullptr;
jclass g_bridge = nullptr;  // global
jmethodID g_on_completed = nullptr;

struct Request {
    jlong id = 0;
    int mask = 0;
    std::wstring reason;
    bool negative_button = false;
    jweak operation = nullptr;  // the Keystore SPI behind the CryptoObject, or null
    std::atomic<bool> cancel{false};
};

std::mutex g_lock;
std::map<jlong, std::shared_ptr<Request>> g_requests;
std::map<jlong, uv::Outcome> g_results;
struct Grant {
    jweak operation;
    int bits;
};
std::vector<Grant> g_grants;            // per-use authorizations, until the operation begins again
ULONGLONG g_last_biometric = 0, g_last_credential = 0;  // GetTickCount64 of the last success, 0 never

std::wstring wide(JNIEnv* env, jstring s) {
    std::wstring out;
    if (!s) return out;
    const jsize n = env->GetStringLength(s);
    out.resize(static_cast<size_t>(n));
    if (n > 0) env->GetStringRegion(s, 0, n, reinterpret_cast<jchar*>(out.data()));
    return out;
}

// The SPI a Cipher, Mac or Signature runs on (its private field, which JNI reads regardless of access):
// the object AndroidKeyStoreAuthPolicy is later given for the same operation.
jobject operation_of(JNIEnv* env, jobject crypto) {
    if (!crypto) return nullptr;
    struct Field {
        const char* name;
        const char* sig;
    };
    static const Field kFields[] = {
        {"spi", "Ljavax/crypto/CipherSpi;"},
        {"spi", "Ljavax/crypto/MacSpi;"},
        {"sigSpi", "Ljava/security/SignatureSpi;"},
    };
    jclass cls = env->GetObjectClass(crypto);
    jobject out = nullptr;
    for (const Field& f : kFields) {
        jfieldID id = env->GetFieldID(cls, f.name, f.sig);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!id) continue;
        out = env->GetObjectField(crypto, id);
        if (out) break;
    }
    env->DeleteLocalRef(cls);
    return out;
}

// Drops grants whose operation was collected. Under g_lock.
void prune_grants(JNIEnv* env) {
    for (size_t i = 0; i < g_grants.size();) {
        if (env->IsSameObject(g_grants[i].operation, nullptr)) {
            env->DeleteWeakGlobalRef(g_grants[i].operation);
            g_grants.erase(g_grants.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

HWND owner_window() {
    HWND w = static_cast<HWND>(window_handle());
    if (w) {
        if (HWND root = GetAncestor(w, GA_ROOT)) w = root;  // embedded in the launcher's panel
    }
    return w ? w : GetForegroundWindow();
}

// Hello's answer to one prompt.
uv::Verification verify(Request* r) {
    Apartment apartment;
    if (!apartment.ok()) return uv::Verification::kFailed;
    HSTRING message = make_string(r->reason);
    if (!message) return uv::Verification::kFailed;
    IAsyncEnum* op = nullptr;
    const HWND window = owner_window();
    if (window) {
        if (IUserConsentVerifierInterop* interop = factory<IUserConsentVerifierInterop>(kIidInterop)) {
            if (FAILED(interop->RequestVerificationForWindowAsync(window, message, kIidAsyncResult,
                                                                  reinterpret_cast<void**>(&op))))
                op = nullptr;
            interop->Release();
        }
    }
    if (!op) {  // no window, or a Windows without the interop: the dialog of its own
        if (IUserConsentVerifierStatics* s = factory<IUserConsentVerifierStatics>(kIidStatics)) {
            if (FAILED(s->RequestVerificationAsync(message, &op))) op = nullptr;
            s->Release();
        }
    }
    combase().delete_string(message);
    if (!op) return uv::Verification::kFailed;
    // The user may take their time: Android's prompt has no timeout either.
    const int v = wait_for(op, &r->cancel, INFINITE);
    op->Release();
    return v >= 0 && v <= 6 ? static_cast<uv::Verification>(v) : uv::Verification::kFailed;
}

void run_prompt(std::shared_ptr<Request> r) {
    const uv::Verification answer = verify(r.get());
    const uv::Host host = host_now();
    const uv::Outcome o = uv::outcome(answer, r->mask, host, r->negative_button);
    std::fprintf(stderr, "[win.apk] user verification: Windows Hello answered %d -> error %d type %d%s\n",
                 static_cast<int>(answer), o.error, o.type, r->cancel.load() ? " (canceled by the app)" : "");
    JNIEnv* env = nullptr;
    const bool attached = g_vm && g_vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), nullptr) == JNI_OK;
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_requests.erase(r->id);
        if (!r->cancel.load()) {
            if (o.error == uv::kSuccess) {
                const ULONGLONG now = GetTickCount64();
                if (o.granted & uv::kStrong) g_last_biometric = now;
                if (o.granted & uv::kCredential) g_last_credential = now;
                if (r->operation && env) {
                    g_grants.push_back({env->NewWeakGlobalRef(r->operation), o.granted});
                }
            }
            g_results[r->id] = o;
        }
        if (r->operation && env) env->DeleteWeakGlobalRef(r->operation);
        r->operation = nullptr;
    }
    if (env && !r->cancel.load() && g_bridge && g_on_completed) {
        env->CallStaticVoidMethod(g_bridge, g_on_completed, r->id);
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    }
    if (attached) g_vm->DetachCurrentThread();
}

// ---- the natives ----------------------------------------------------------------------------

jboolean JNICALL Bridge_bound(JNIEnv*, jclass) { return combase().ok() ? JNI_TRUE : JNI_FALSE; }

jint JNICALL Bridge_canAuthenticate(JNIEnv*, jclass, jint mask) { return uv::can_authenticate(mask, host_now()); }

jboolean JNICALL Bridge_begin(JNIEnv* env, jclass cls, jlong id, jint mask, jstring reason, jobject crypto,
                              jstring negative) {
    if (!combase().ok()) return JNI_FALSE;
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (!g_vm && env->GetJavaVM(&g_vm) != JNI_OK) return JNI_FALSE;
        if (!g_bridge) {
            g_bridge = static_cast<jclass>(env->NewGlobalRef(cls));
            g_on_completed = env->GetStaticMethodID(cls, "onNativeCompleted", "(J)V");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (!g_on_completed) return JNI_FALSE;
    }
    auto r = std::make_shared<Request>();
    r->id = id;
    r->mask = mask;
    r->reason = wide(env, reason);
    r->negative_button = negative != nullptr && env->GetStringLength(negative) > 0;
    if (crypto) {
        jobject op = operation_of(env, crypto);
        if (op) {
            r->operation = env->NewWeakGlobalRef(op);
            env->DeleteLocalRef(op);
        }
    }
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_requests[id] = r;
    }
    std::thread(run_prompt, r).detach();
    return JNI_TRUE;
}

void JNICALL Bridge_cancel(JNIEnv*, jclass, jlong id) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_requests.find(id);
    if (it != g_requests.end()) it->second->cancel = true;
    g_results.erase(id);
}

jintArray JNICALL Bridge_takeResult(JNIEnv* env, jclass, jlong id) {
    uv::Outcome o;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_results.find(id);
        if (it == g_results.end()) return nullptr;
        o = it->second;
        g_results.erase(it);
    }
    jintArray out = env->NewIntArray(2);
    if (!out) return nullptr;
    const jint values[2] = {o.error, o.type};
    env->SetIntArrayRegion(out, 0, 2, values);
    return out;
}

// SystemClock.elapsedRealtime() of the last success with one of `mask`'s authenticators, or -1.
jlong JNICALL Bridge_lastAuthenticationTime(JNIEnv* env, jclass, jint mask) {
    ULONGLONG last = 0;
    {
        std::lock_guard<std::mutex> l(g_lock);
        if ((mask & uv::kStrong) && g_last_biometric > last) last = g_last_biometric;
        if ((mask & uv::kCredential) && g_last_credential > last) last = g_last_credential;
    }
    if (last == 0) return -1;
    jclass clock = env->FindClass("android/os/SystemClock");
    if (!clock) {
        env->ExceptionClear();
        return -1;
    }
    jmethodID elapsed = env->GetStaticMethodID(clock, "elapsedRealtime", "()J");
    jlong now = elapsed ? env->CallStaticLongMethod(clock, elapsed) : -1;
    env->DeleteLocalRef(clock);
    if (env->ExceptionCheck() || now < 0) {
        env->ExceptionClear();
        return -1;
    }
    const jlong age = static_cast<jlong>(GetTickCount64() - last);
    return now > age ? now - age : 0;
}

// A Keystore operation begins (or ends): a prompt before it no longer authorizes it.
void JNICALL Bridge_beginKeyOperation(JNIEnv* env, jclass, jobject operation) {
    if (!operation) return;
    std::lock_guard<std::mutex> l(g_lock);
    for (size_t i = 0; i < g_grants.size();) {
        if (env->IsSameObject(g_grants[i].operation, operation)) {
            env->DeleteWeakGlobalRef(g_grants[i].operation);
            g_grants.erase(g_grants.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    prune_grants(env);
}

jboolean JNICALL Bridge_consumeAuthorization(JNIEnv* env, jclass, jobject operation, jboolean per_use, jint seconds,
                                             jint mask) {
    if (!operation) return JNI_FALSE;
    std::lock_guard<std::mutex> l(g_lock);
    if (per_use) {
        prune_grants(env);
        for (const Grant& g : g_grants)
            if ((g.bits & mask) != 0 && env->IsSameObject(g.operation, operation)) return JNI_TRUE;
        return JNI_FALSE;
    }
    return uv::within(mask, g_last_biometric, g_last_credential, GetTickCount64(), seconds) ? JNI_TRUE : JNI_FALSE;
}

#define BRIDGE "com.macapk.loader.UserVerificationBridge"
const HostMethod kMethods[] = {
    WINAPK_HOST_NATIVE(BRIDGE, "nativeBound", "()Z", Bridge_bound),
    WINAPK_HOST_NATIVE(BRIDGE, "nativeCanAuthenticate", "(I)I", Bridge_canAuthenticate),
    WINAPK_HOST_NATIVE(BRIDGE, "nativeBegin", "(JILjava/lang/String;Ljava/lang/Object;Ljava/lang/String;)Z",
                       Bridge_begin),
    WINAPK_HOST_NATIVE(BRIDGE, "nativeCancel", "(J)V", Bridge_cancel),
    WINAPK_HOST_NATIVE(BRIDGE, "nativeTakeResult", "(J)[I", Bridge_takeResult),
    WINAPK_HOST_NATIVE(BRIDGE, "nativeLastAuthenticationTime", "(I)J", Bridge_lastAuthenticationTime),
    WINAPK_HOST_NATIVE(BRIDGE, "nativeBeginKeyOperation", "(Ljava/lang/Object;)V", Bridge_beginKeyOperation),
    WINAPK_HOST_NATIVE(BRIDGE, "nativeConsumeAuthorization", "(Ljava/lang/Object;ZII)Z",
                       Bridge_consumeAuthorization),
};
#undef BRIDGE

}  // namespace

HostMethods native_user_verification_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

}  // namespace winapk::host
