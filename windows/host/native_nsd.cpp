// native_nsd.cpp -- com.macapk.loader.HostNsd: network service discovery (DNS-SD over mDNS) on Windows' own
// responder, for android.net.nsd.NsdManager. Windows 10 (1809 on) answers and asks mDNS itself, on port
// 5353, which an app's own responder could not share: the DnsService* functions of dnsapi.dll are how a
// program browses, resolves and registers services there (DnsServiceBrowse, DnsServiceResolve,
// DnsServiceRegister). They are looked up at run time, so an older Windows answers "not available" (the
// Java side then reports FAILURE_INTERNAL_ERROR, as before) instead of the runtime not starting.
//
// Each call is synchronous for its Java caller, which runs it on NsdManager's own threads: a browse
// collects the instances the network names within its time, a resolve waits for its one answer, a register
// for Windows' confirmation. Windows calls back on its own threads; each request's state is shared with the
// callback and freed by whichever side is the last to let it go (a cancelled request still gets its
// callback, with ERROR_CANCELLED).

#include <windows.h>

#include <windns.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "host_natives.h"

namespace winapk::host {

namespace {

using BrowseFn = DNS_STATUS(WINAPI*)(PDNS_SERVICE_BROWSE_REQUEST, PDNS_SERVICE_CANCEL);
using CancelFn = DNS_STATUS(WINAPI*)(PDNS_SERVICE_CANCEL);
using ResolveFn = DNS_STATUS(WINAPI*)(PDNS_SERVICE_RESOLVE_REQUEST, PDNS_SERVICE_CANCEL);
using RegisterFn = DWORD(WINAPI*)(PDNS_SERVICE_REGISTER_REQUEST, PDNS_SERVICE_CANCEL);
using ConstructFn = PDNS_SERVICE_INSTANCE(WINAPI*)(PCWSTR, PCWSTR, PIP4_ADDRESS, PIP6_ADDRESS, WORD, WORD, WORD,
                                                  DWORD, PCWSTR*, PCWSTR*);
using FreeInstanceFn = VOID(WINAPI*)(PDNS_SERVICE_INSTANCE);
using RecordListFreeFn = VOID(WINAPI*)(PDNS_RECORD, DNS_FREE_TYPE);

struct Api {
    BrowseFn browse = nullptr;
    CancelFn browse_cancel = nullptr;
    ResolveFn resolve = nullptr;
    CancelFn resolve_cancel = nullptr;
    RegisterFn reg = nullptr;
    RegisterFn dereg = nullptr;
    ConstructFn construct = nullptr;
    FreeInstanceFn free_instance = nullptr;
    RecordListFreeFn record_list_free = nullptr;
    bool ok = false;
};

const Api& api() {
    static const Api a = [] {
        Api r;
        HMODULE dll = LoadLibraryExW(L"dnsapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll) return r;
        auto get = [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(dll, name)); };
        r.browse = reinterpret_cast<BrowseFn>(get("DnsServiceBrowse"));
        r.browse_cancel = reinterpret_cast<CancelFn>(get("DnsServiceBrowseCancel"));
        r.resolve = reinterpret_cast<ResolveFn>(get("DnsServiceResolve"));
        r.resolve_cancel = reinterpret_cast<CancelFn>(get("DnsServiceResolveCancel"));
        r.reg = reinterpret_cast<RegisterFn>(get("DnsServiceRegister"));
        r.dereg = reinterpret_cast<RegisterFn>(get("DnsServiceDeRegister"));
        r.construct = reinterpret_cast<ConstructFn>(get("DnsServiceConstructInstance"));
        r.free_instance = reinterpret_cast<FreeInstanceFn>(get("DnsServiceFreeInstance"));
        r.record_list_free = reinterpret_cast<RecordListFreeFn>(get("DnsRecordListFree"));
        r.ok = r.browse && r.browse_cancel && r.resolve && r.resolve_cancel && r.reg && r.dereg && r.construct &&
               r.free_instance && r.record_list_free;
        if (!r.ok) {
            std::fprintf(stderr, "[win.apk] NSD: this Windows has no DNS-SD service functions (Windows 10 1809 on)\n");
        }
        return r;
    }();
    return a;
}

std::wstring wide(JNIEnv* env, jstring s) {
    if (!s) return {};
    const jchar* c = env->GetStringChars(s, nullptr);
    std::wstring w(reinterpret_cast<const wchar_t*>(c), static_cast<size_t>(env->GetStringLength(s)));
    env->ReleaseStringChars(s, c);
    return w;
}

jstring jstr(JNIEnv* env, const wchar_t* w) {
    if (!w) w = L"";
    return env->NewString(reinterpret_cast<const jchar*>(w), static_cast<jsize>(wcslen(w)));
}

/** A request's state, shared by its caller and Windows' callback: freed by the last of the two. */
struct Shared {
    std::atomic<int> refs{2};
    std::mutex lock;
    std::condition_variable changed;
    bool done = false;
    DWORD status = ERROR_SUCCESS;
    std::vector<std::wstring> names;      // a browse's instances
    std::vector<std::wstring> resolved;   // a resolve's host, port, ip4, ip6, keys and values
    std::wstring registered;              // a register's final instance name

    void release() {
        if (--refs == 0) delete this;
    }
};

std::wstring ip4_text(const IP4_ADDRESS* a) {
    if (!a) return {};
    const BYTE* b = reinterpret_cast<const BYTE*>(a);
    wchar_t buf[24];
    swprintf(buf, 24, L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

std::wstring ip6_text(const IP6_ADDRESS* a) {
    if (!a) return {};
    wchar_t buf[64];
    std::wstring out;
    for (int i = 0; i < 8; ++i) {
        swprintf(buf, 64, i ? L":%x" : L"%x", (a->IP6Byte[2 * i] << 8) | a->IP6Byte[2 * i + 1]);
        out += buf;
    }
    return out;
}

VOID WINAPI on_browse(DWORD status, PVOID context, PDNS_RECORD records) {
    auto* s = static_cast<Shared*>(context);
    {
        std::lock_guard<std::mutex> hold(s->lock);
        // The DNS-SD functions are Unicode only: their records are DNS_RECORDW whatever UNICODE says.
        for (auto* r = reinterpret_cast<PDNS_RECORDW>(records); r; r = r->pNext) {
            if (r->wType == DNS_TYPE_PTR && r->Data.PTR.pNameHost) s->names.emplace_back(r->Data.PTR.pNameHost);
        }
        if (status == ERROR_CANCELLED) s->done = true;
    }
    if (records) api().record_list_free(records, DnsFreeRecordList);
    s->changed.notify_all();
    if (status == ERROR_CANCELLED) s->release();
}

VOID WINAPI on_resolve(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance) {
    auto* s = static_cast<Shared*>(context);
    {
        std::lock_guard<std::mutex> hold(s->lock);
        s->status = status;
        if (status == ERROR_SUCCESS && instance) {
            s->resolved.emplace_back(instance->pszHostName ? instance->pszHostName : L"");
            s->resolved.emplace_back(std::to_wstring(instance->wPort));
            s->resolved.emplace_back(ip4_text(instance->ip4Address));
            s->resolved.emplace_back(ip6_text(instance->ip6Address));
            for (DWORD i = 0; i < instance->dwPropertyCount; ++i) {
                s->resolved.emplace_back(instance->keys && instance->keys[i] ? instance->keys[i] : L"");
                s->resolved.emplace_back(instance->values && instance->values[i] ? instance->values[i] : L"");
            }
        }
        s->done = true;
    }
    if (instance) api().free_instance(instance);
    s->changed.notify_all();
    s->release();
}

VOID WINAPI on_register(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance) {
    auto* s = static_cast<Shared*>(context);
    {
        std::lock_guard<std::mutex> hold(s->lock);
        s->status = status;
        if (instance && instance->pszInstanceName) s->registered = instance->pszInstanceName;
        s->done = true;
    }
    if (instance) api().free_instance(instance);
    s->changed.notify_all();
    s->release();
}

/** The instances of `query` ("_http._tcp.local") the network names within `ms`; null when Windows has no
 *  DNS-SD or refused the browse. */
jobjectArray JNICALL Nsd_browse(JNIEnv* env, jclass, jstring query, jint ms) {
    const Api& a = api();
    if (!a.ok) return nullptr;
    std::wstring name = wide(env, query);
    auto* s = new Shared();
    DNS_SERVICE_BROWSE_REQUEST req{};
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.QueryName = name.c_str();
    req.pBrowseCallback = &on_browse;
    req.pQueryContext = s;
    DNS_SERVICE_CANCEL cancel{};
    if (a.browse(&req, &cancel) != DNS_REQUEST_PENDING) {
        delete s;
        return nullptr;
    }
    {
        std::unique_lock<std::mutex> hold(s->lock);
        s->changed.wait_for(hold, std::chrono::milliseconds(ms), [&] { return s->done; });
    }
    a.browse_cancel(&cancel);
    std::vector<std::wstring> names;
    {
        std::lock_guard<std::mutex> hold(s->lock);
        names = s->names;
    }
    s->release();
    jobjectArray out = env->NewObjectArray(static_cast<jsize>(names.size()), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < names.size(); ++i) {
        jstring j = jstr(env, names[i].c_str());
        env->SetObjectArrayElement(out, static_cast<jsize>(i), j);
        env->DeleteLocalRef(j);
    }
    return out;
}

/** One instance ("My Printer._http._tcp.local") resolved: {host, port, ip4, ip6, key, value...}, or null. */
jobjectArray JNICALL Nsd_resolve(JNIEnv* env, jclass, jstring instance, jint ms) {
    const Api& a = api();
    if (!a.ok) return nullptr;
    std::wstring name = wide(env, instance);
    auto* s = new Shared();
    DNS_SERVICE_RESOLVE_REQUEST req{};
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.QueryName = &name[0];
    req.pResolveCompletionCallback = &on_resolve;
    req.pQueryContext = s;
    DNS_SERVICE_CANCEL cancel{};
    if (a.resolve(&req, &cancel) != DNS_REQUEST_PENDING) {
        delete s;
        return nullptr;
    }
    bool answered;
    {
        std::unique_lock<std::mutex> hold(s->lock);
        answered = s->changed.wait_for(hold, std::chrono::milliseconds(ms), [&] { return s->done; });
    }
    if (!answered) a.resolve_cancel(&cancel);
    std::vector<std::wstring> got;
    DWORD status;
    {
        std::lock_guard<std::mutex> hold(s->lock);
        got = s->resolved;
        status = s->status;
    }
    s->release();
    if (!answered || status != ERROR_SUCCESS || got.empty()) return nullptr;
    jobjectArray out = env->NewObjectArray(static_cast<jsize>(got.size()), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < got.size(); ++i) {
        jstring j = jstr(env, got[i].c_str());
        env->SetObjectArrayElement(out, static_cast<jsize>(i), j);
        env->DeleteLocalRef(j);
    }
    return out;
}

/** A registered service: its request, which DnsServiceDeRegister takes back. */
struct Registration {
    PDNS_SERVICE_INSTANCE instance = nullptr;
    std::wstring name;
};

/** Registers `instance` ("Name._type._tcp.local") at this computer's mDNS host name and `port`, with its TXT
 *  record; waits up to `ms` for Windows to confirm. A handle (0: refused), and out[0] the name Windows
 *  registered (it renames on a conflict, as an Android device's responder does). */
jlong JNICALL Nsd_register(JNIEnv* env, jclass, jstring instance, jint port, jobjectArray keys, jobjectArray values,
                           jint ms, jobjectArray out) {
    const Api& a = api();
    if (!a.ok) return 0;
    std::wstring name = wide(env, instance);
    wchar_t computer[256];
    DWORD size = 256;
    if (!GetComputerNameExW(ComputerNameDnsHostname, computer, &size)) return 0;
    std::wstring host = std::wstring(computer) + L".local";
    const jsize n = keys ? env->GetArrayLength(keys) : 0;
    std::vector<std::wstring> k(n), v(n);
    std::vector<PCWSTR> kp(n), vp(n);
    for (jsize i = 0; i < n; ++i) {
        auto ks = static_cast<jstring>(env->GetObjectArrayElement(keys, i));
        auto vs = static_cast<jstring>(env->GetObjectArrayElement(values, i));
        k[i] = wide(env, ks);
        v[i] = wide(env, vs);
        kp[i] = k[i].c_str();
        vp[i] = v[i].c_str();
    }
    auto* reg = new Registration();
    reg->instance = a.construct(name.c_str(), host.c_str(), nullptr, nullptr, static_cast<WORD>(port), 0, 0,
                                static_cast<DWORD>(n), n ? kp.data() : nullptr, n ? vp.data() : nullptr);
    if (!reg->instance) {
        delete reg;
        return 0;
    }
    auto* s = new Shared();
    DNS_SERVICE_REGISTER_REQUEST req{};
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.pServiceInstance = reg->instance;
    req.pRegisterCompletionCallback = &on_register;
    req.pQueryContext = s;
    DNS_SERVICE_CANCEL cancel{};
    if (a.reg(&req, &cancel) != DNS_REQUEST_PENDING) {
        delete s;
        a.free_instance(reg->instance);
        delete reg;
        return 0;
    }
    bool answered;
    {
        std::unique_lock<std::mutex> hold(s->lock);
        answered = s->changed.wait_for(hold, std::chrono::milliseconds(ms), [&] { return s->done; });
    }
    DWORD status;
    std::wstring registered;
    {
        std::lock_guard<std::mutex> hold(s->lock);
        status = s->status;
        registered = s->registered;
    }
    s->release();
    if (!answered || status != ERROR_SUCCESS) {
        // Not confirmed in time: what Windows may still be doing is taken back.
        DNS_SERVICE_REGISTER_REQUEST undo = req;
        auto* u = new Shared();
        undo.pQueryContext = u;
        if (a.dereg(&undo, nullptr) == DNS_REQUEST_PENDING) {
            // Windows holds the instance until it answers: it is left to the process' end, as in
            // Nsd_unregister when no answer comes.
            u->release();
        } else {
            delete u;
            a.free_instance(reg->instance);
            delete reg;
        }
        return 0;
    }
    reg->name = registered.empty() ? name : registered;
    if (out && env->GetArrayLength(out) > 0) {
        jstring j = jstr(env, reg->name.c_str());
        env->SetObjectArrayElement(out, 0, j);
        env->DeleteLocalRef(j);
    }
    return reinterpret_cast<jlong>(reg);
}

void JNICALL Nsd_unregister(JNIEnv*, jclass, jlong handle) {
    auto* reg = reinterpret_cast<Registration*>(handle);
    if (!reg) return;
    const Api& a = api();
    auto* s = new Shared();
    DNS_SERVICE_REGISTER_REQUEST req{};
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.pServiceInstance = reg->instance;
    req.pRegisterCompletionCallback = &on_register;
    req.pQueryContext = s;
    bool confirmed;
    if (a.dereg(&req, nullptr) == DNS_REQUEST_PENDING) {
        // Its goodbye is sent; the confirmation is waited for briefly so the instance outlives the request.
        std::unique_lock<std::mutex> hold(s->lock);
        confirmed = s->changed.wait_for(hold, std::chrono::milliseconds(2000), [&] { return s->done; });
        hold.unlock();
        s->release();
    } else {
        confirmed = true;  // nothing pending reads it
        delete s;
    }
    // The instance is freed only once Windows answered (else it may still read it): left to the process'
    // end otherwise, a few hundred bytes for a registration taken back.
    if (confirmed) {
        a.free_instance(reg->instance);
        delete reg;
    }
}

jboolean JNICALL Nsd_available(JNIEnv*, jclass) { return api().ok ? JNI_TRUE : JNI_FALSE; }

constexpr const char* kCls = "com.macapk.loader.HostNsd";

const HostMethod kMethods[] = {
    WINAPK_HOST_NATIVE(kCls, "nativeAvailable", "()Z", Nsd_available),
    WINAPK_HOST_NATIVE(kCls, "nativeBrowse", "(Ljava/lang/String;I)[Ljava/lang/String;", Nsd_browse),
    WINAPK_HOST_NATIVE(kCls, "nativeResolve", "(Ljava/lang/String;I)[Ljava/lang/String;", Nsd_resolve),
    WINAPK_HOST_NATIVE(kCls, "nativeRegister",
                       "(Ljava/lang/String;I[Ljava/lang/String;[Ljava/lang/String;I[Ljava/lang/String;)J",
                       Nsd_register),
    WINAPK_HOST_NATIVE(kCls, "nativeUnregister", "(J)V", Nsd_unregister),
};

}  // namespace

HostMethods native_nsd_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

}  // namespace winapk::host
