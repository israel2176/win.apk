// native_notifications.cpp -- com.macapk.loader.HostNotifications on Windows:
// what an app posts through NotificationManager.notify, shown as Windows
// notifications (Windows.UI.Notifications toasts) with the app's own name and
// icon, kept in the Action Center until dismissed or cancelled. Upstream shows
// nothing on the Mac.
//
// A classic Win32 program may post toasts once it has an AppUserModelID that a
// Start menu shortcut carries (System.AppUserModel.ID): the launcher writes
// each app's shortcut with it (make_shortcut in launcher_main.cpp), and
// set_app_user_model_id gives this process the same id before its window
// exists, so Windows takes the toast's name and icon from that shortcut. The
// id is derived from the app's library name, the same way in both programs.
//
// WinRT through its ABI, declared here, as native_location.cpp does: MinGW's
// headers lack Windows.UI.Notifications and Windows.Data.Xml.Dom; combase is
// loaded at run time. The toast objects are agile, so they are used on
// whichever Java thread notifies (RoInitialize once per thread, under one
// lock). A click arrives on the thread pool: the app's window is brought to
// the front and HostNotifications.onActivated sends the notification's
// contentIntent on the main thread, as a tap on a device does. That works
// while the app runs; a notification left in the Action Center by an app that
// has ended does nothing when clicked.
//
// What a toast carries (the Java side already laid the text out as Android
// does): the title, the text, the sub text as the attribution line, the large
// icon as the picture at the left, a progress bar where the notification has
// one. With `popup` (importance DEFAULT and above, not a re-post with
// FLAG_ONLY_ALERT_ONCE) the toast shows as a banner with the notification
// sound; without it (LOW and MIN) it joins the Action Center quietly, as such
// notifications join the shade. The notification's actions are the toast's
// buttons (toast_xml.h): a click on one sends that action's PendingIntent
// (HostNotifications.onAction), with the text typed into its field when the
// action takes a free-form RemoteInput, and does not bring the app's window
// forward, as an action on a device opens nothing unless its intent does. A
// key notified again replaces its toast (the
// same tag and group), and cancel removes it from the screen and the Action
// Center, also for a previous run's.

#include "host_natives.h"
#include "toast_xml.h"
#include "win32_window.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <inspectable.h>
#include <shobjidl.h>
#include <unknwn.h>

namespace winapk::host {

namespace {

// ---- the WinRT ABI we use ------------------------------------------------------

struct EventToken { INT64 value; };

// Windows.Data.Xml.Dom.XmlDocument, loaded from text. The document itself
// (IXmlDocument) is only handed on, so its methods are not declared.
struct IXmlDocumentIO : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE LoadXml(HSTRING xml) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadXmlWithSettings(HSTRING xml, IInspectable* settings) = 0;
    virtual HRESULT STDMETHODCALLTYPE SaveToFileAsync(IInspectable* file, IInspectable** action) = 0;
};

struct IToastNotification : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Content(IInspectable** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_ExpirationTime(IInspectable* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ExpirationTime(IInspectable** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Dismissed(IUnknown* handler, EventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Dismissed(EventToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Activated(IUnknown* handler, EventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Activated(EventToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Failed(IUnknown* handler, EventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Failed(EventToken token) = 0;
};

// Windows 10: the tag and group a toast is found by again, and SuppressPopup.
struct IToastNotification2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Tag(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Tag(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Group(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Group(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_SuppressPopup(unsigned char value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SuppressPopup(unsigned char* value) = 0;
};

struct IToastNotificationFactory : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateToastNotification(IInspectable* content, IToastNotification** value) = 0;
};

// Only Show, Hide and Setting are called; the schedule methods follow them.
struct IToastNotifier : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Show(IToastNotification* notification) = 0;
    virtual HRESULT STDMETHODCALLTYPE Hide(IToastNotification* notification) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Setting(int* value) = 0;
};

struct IToastNotificationHistory : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RemoveGroup(HSTRING group) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveGroupWithId(HSTRING group, HSTRING app_id) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveGroupedTagWithId(HSTRING tag, HSTRING group, HSTRING app_id) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveGroupedTag(HSTRING tag, HSTRING group) = 0;
    virtual HRESULT STDMETHODCALLTYPE Remove(HSTRING tag) = 0;
    virtual HRESULT STDMETHODCALLTYPE Clear() = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearWithId(HSTRING app_id) = 0;
};

struct IToastNotificationManagerStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateToastNotifier(IToastNotifier** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateToastNotifierWithId(HSTRING app_id, IToastNotifier** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTemplateContent(int type, IInspectable** value) = 0;
};

struct IToastNotificationManagerStatics2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_History(IToastNotificationHistory** value) = 0;
};

// What a click hands the Activated event: the arguments of what was clicked
// (the toast's launch, or an action's), and the text of its inputs.
struct IToastActivatedEventArgs : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Arguments(HSTRING* value) = 0;
};
struct IToastActivatedEventArgs2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_UserInput(IInspectable** value) = 0;  // a ValueSet
};
// IMap<String, Object>, which a ValueSet is; only Lookup is called.
struct IMapStringObject : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Lookup(HSTRING key, IInspectable** value) = 0;
};
// Windows.Foundation.IPropertyValue, the boxed string an input's text is; GetString is the 14th.
struct IPropertyValue : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Type(int* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsNumericScalar(unsigned char* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetUInt8(unsigned char* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInt16(INT16* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetUInt16(UINT16* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInt32(INT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetUInt32(UINT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInt64(INT64* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetUInt64(UINT64* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSingle(float* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDouble(double* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChar16(wchar_t* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBoolean(unsigned char* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetString(HSTRING* value) = 0;
};

// From the SDK headers (Windows.UI.Notifications, Windows.Data.Xml.Dom, Windows.Foundation).
constexpr GUID kIidManagerStatics = {0x50ac103f, 0xd235, 0x4598, {0xbb, 0xef, 0x98, 0xfe, 0x4d, 0x1a, 0x3a, 0xd4}};
constexpr GUID kIidManagerStatics2 = {0x7ab93c52, 0x0e48, 0x4750, {0xba, 0x9d, 0x1a, 0x41, 0x13, 0x98, 0x18, 0x47}};
constexpr GUID kIidToastFactory = {0x04124b20, 0x82c6, 0x4229, {0xb1, 0x09, 0xfd, 0x9e, 0xd4, 0x66, 0x2b, 0x53}};
constexpr GUID kIidToastNotification = {0x997e2675, 0x059e, 0x4e60, {0x8b, 0x06, 0x17, 0x60, 0x91, 0x7c, 0x8b, 0x80}};
constexpr GUID kIidToastNotification2 = {0x9dfb9fd1, 0x143a, 0x490e, {0x90, 0xbf, 0xb9, 0xfb, 0xa7, 0x13, 0x2d, 0xe7}};
constexpr GUID kIidXmlDocumentIO = {0x6cd0e74e, 0xee65, 0x4489, {0x9e, 0xbf, 0xca, 0x43, 0xe8, 0x7b, 0xa6, 0x37}};
constexpr GUID kIidActivatedArgs = {0xe3bf92f3, 0xc197, 0x436f, {0x82, 0x65, 0x06, 0x25, 0x82, 0x4f, 0x8d, 0xac}};
constexpr GUID kIidActivatedArgs2 = {0xab7da512, 0xcc61, 0x568e, {0x81, 0xbe, 0x30, 0x4a, 0xc3, 0x10, 0x38, 0xfa}};
constexpr GUID kIidMapStringObject = {0x1b0d3570, 0x0877, 0x5ec2, {0x8a, 0x2c, 0x3b, 0x95, 0x39, 0x50, 0x6a, 0xca}};
constexpr GUID kIidPropertyValue = {0x4bd682dd, 0x7554, 0x40e9, {0x9a, 0x9b, 0x82, 0x65, 0x4e, 0xde, 0x7e, 0x62}};
constexpr GUID kIidAgileObject = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
constexpr GUID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
// TypedEventHandler<ToastNotification, IInspectable> (the Activated event):
// a parameterized IID, derived from the signature as the WinRT type system does.
constexpr GUID kIidActivatedHandler = {0xab54de2d, 0x97d9, 0x5528, {0xb6, 0xad, 0x10, 0x5a, 0xfe, 0x15, 0x65, 0x30}};

constexpr int kRoInitMultithreaded = 1;
// NotificationSetting.Enabled; the others say who turned them off.
constexpr int kSettingEnabled = 0;

const wchar_t* const kGroup = L"winapk";

// ---- combase, loaded at run time ---------------------------------------------------

using RoInitializeFn = HRESULT(WINAPI*)(int);
using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
using RoActivateInstanceFn = HRESULT(WINAPI*)(HSTRING, IInspectable**);
using WindowsCreateStringFn = HRESULT(WINAPI*)(const wchar_t*, UINT32, HSTRING*);
using WindowsDeleteStringFn = HRESULT(WINAPI*)(HSTRING);
using WindowsGetStringRawBufferFn = PCWSTR(WINAPI*)(HSTRING, UINT32*);

template <typename F>
F proc(HMODULE m, const char* name) {
    return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

struct Combase {
    RoInitializeFn ro_initialize = nullptr;
    RoGetActivationFactoryFn get_factory = nullptr;
    RoActivateInstanceFn activate = nullptr;
    WindowsCreateStringFn create_string = nullptr;
    WindowsDeleteStringFn delete_string = nullptr;
    WindowsGetStringRawBufferFn raw_buffer = nullptr;

    bool load() {
        HMODULE m = LoadLibraryW(L"combase.dll");
        if (!m) return false;
        ro_initialize = proc<RoInitializeFn>(m, "RoInitialize");
        get_factory = proc<RoGetActivationFactoryFn>(m, "RoGetActivationFactory");
        activate = proc<RoActivateInstanceFn>(m, "RoActivateInstance");
        create_string = proc<WindowsCreateStringFn>(m, "WindowsCreateString");
        delete_string = proc<WindowsDeleteStringFn>(m, "WindowsDeleteString");
        raw_buffer = proc<WindowsGetStringRawBufferFn>(m, "WindowsGetStringRawBuffer");
        return ro_initialize && get_factory && activate && create_string && delete_string && raw_buffer;
    }
};

Combase g_combase;

// An HSTRING for the length of a call.
class HStr {
public:
    explicit HStr(const std::wstring& s) {
        if (FAILED(g_combase.create_string(s.c_str(), static_cast<UINT32>(s.size()), &h_))) h_ = nullptr;
    }
    ~HStr() {
        if (h_) g_combase.delete_string(h_);
    }
    HSTRING get() const { return h_; }
    HStr(const HStr&) = delete;
    HStr& operator=(const HStr&) = delete;

private:
    HSTRING h_ = nullptr;
};

template <typename T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// ---- the shared state ----------------------------------------------------------------

std::mutex g_lock;  // everything below, and every call into the toast objects
int g_state = 0;    // 0 not started, 1 ready, 2 failed (logged once)
std::wstring g_aumid;
IToastNotifier* g_notifier = nullptr;
IToastNotificationFactory* g_factory = nullptr;
IToastNotificationHistory* g_history = nullptr;  // null before Windows 10
std::map<std::wstring, IToastNotification*> g_shown;  // by key: the toast to hide or replace
JavaVM* g_vm = nullptr;
jclass g_bridge = nullptr;  // global ref to HostNotifications
jmethodID g_on_activated = nullptr;
jmethodID g_on_action = nullptr;

void log(const char* what, HRESULT hr) {
    std::fprintf(stderr, "[win.apk] notifications: %s (0x%08lx)\n", what, static_cast<unsigned long>(hr));
    std::fflush(stderr);
}

// Kept in step with the launcher's app_user_model_id (launcher_main.cpp), which
// writes the shortcut: the two strings must be the same. Letters, digits, '.',
// '-' and '_' pass; anything else becomes '_' and a hash of the whole name is
// appended, so names that differ only there stay apart; at most 128
// characters and no spaces, as the id's rules say.
std::wstring app_user_model_id(const std::wstring& name) {
    std::wstring id = L"WinApk.App.";
    bool mangled = false;
    uint64_t hash = 14695981039346656037ULL;  // FNV-1a
    for (wchar_t c : name) {
        hash = (hash ^ static_cast<uint64_t>(c)) * 1099511628211ULL;
        const bool plain = (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
                           c == L'.' || c == L'-' || c == L'_';
        id += plain ? c : L'_';
        mangled = mangled || !plain;
    }
    if (mangled || id.size() > 100) {
        id = id.substr(0, 100) + L'.';
        for (int i = 60; i >= 0; i -= 4) id += L"0123456789abcdef"[(hash >> i) & 0xf];
    }
    return id;
}

// The toast's tag for a key: 16 hex digits of its hash, the most a tag could
// hold before Windows 10 1903 (a key holds the app's own tag, any length).
std::wstring tag_of(const std::wstring& key) {
    uint64_t hash = 14695981039346656037ULL;
    for (wchar_t c : key) hash = (hash ^ static_cast<uint64_t>(c)) * 1099511628211ULL;
    std::wstring tag;
    for (int i = 60; i >= 0; i -= 4) tag += L"0123456789abcdef"[(hash >> i) & 0xf];
    return tag;
}

// WinRT on the calling thread (any Java thread may notify): the multithreaded
// apartment, or whatever apartment the thread already has (RPC_E_CHANGED_MODE).
bool ro_init_thread() {
    thread_local bool done = false;
    if (done) return true;
    const HRESULT hr = g_combase.ro_initialize(kRoInitMultithreaded);
    done = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    if (!done) log("RoInitialize failed", hr);
    return done;
}

JNIEnv* java_env() {
    if (!g_vm) return nullptr;
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (g_vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), nullptr) != JNI_OK) return nullptr;
    return env;
}

std::wstring wide(JNIEnv* env, jstring s) {
    if (!s) return {};
    const jchar* c = env->GetStringChars(s, nullptr);
    std::wstring w(reinterpret_cast<const wchar_t*>(c), static_cast<size_t>(env->GetStringLength(s)));
    env->ReleaseStringChars(s, c);
    return w;
}

// Where the large icons go: <app dir>\notifications\<tag>.png.
std::wstring icons_dir() {
    const std::wstring dir = app_dir();
    return dir.empty() ? dir : dir + L"\\notifications";
}

std::wstring icon_path(const std::wstring& tag) {
    const std::wstring dir = icons_dir();
    return dir.empty() ? dir : dir + L"\\" + tag + L".png";
}

bool write_file(const std::wstring& path, const void* data, size_t size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
    CloseHandle(f);
    return ok;
}

void remove_icons() {
    const std::wstring dir = icons_dir();
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Drops the toast of `key` from g_shown, when it is still `toast` (a newer one
// may have replaced it): the user clicked it, or it was cancelled.
void forget(const std::wstring& key, IToastNotification* toast) {
    std::lock_guard<std::mutex> hold(g_lock);
    auto it = g_shown.find(key);
    if (it == g_shown.end() || (toast && it->second != toast)) return;
    it->second->Release();
    g_shown.erase(it);
}

// The user clicked the toast of `key`: the app's window to the front (a
// minimized one restored), then the Java side sends the contentIntent.
void activate(const std::wstring& key) {
    if (HWND h = static_cast<HWND>(window_handle())) {
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        SetForegroundWindow(h);
    }
    JNIEnv* env = java_env();
    if (!env || !g_bridge || !g_on_activated) return;
    jstring k = env->NewString(reinterpret_cast<const jchar*>(key.data()), static_cast<jsize>(key.size()));
    if (k) {
        env->CallStaticVoidMethod(g_bridge, g_on_activated, k);
        env->DeleteLocalRef(k);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

// The user clicked action `index` of the toast of `key`, `reply` the text of its field (null: it
// has none): the Java side sends the action's PendingIntent. The window stays where it is.
void act(const std::wstring& key, int index, const std::wstring* reply) {
    JNIEnv* env = java_env();
    if (!env || !g_bridge || !g_on_action) return;
    jstring k = env->NewString(reinterpret_cast<const jchar*>(key.data()), static_cast<jsize>(key.size()));
    jstring text = reply ? env->NewString(reinterpret_cast<const jchar*>(reply->data()), static_cast<jsize>(reply->size()))
                         : nullptr;
    if (k) env->CallStaticVoidMethod(g_bridge, g_on_action, k, static_cast<jint>(index), text);
    if (text) env->DeleteLocalRef(text);
    if (k) env->DeleteLocalRef(k);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

std::wstring take_string(HSTRING h) {
    if (!h) return {};
    UINT32 n = 0;
    const wchar_t* p = g_combase.raw_buffer(h, &n);
    std::wstring s = p ? std::wstring(p, n) : std::wstring();
    g_combase.delete_string(h);
    return s;
}

// What the click names (the toast or one of its actions) and the text of the clicked action's
// field, from the Activated event's arguments.
bool read_click(IInspectable* args, std::wstring* key, int* action, bool* has_reply, std::wstring* reply) {
    *has_reply = false;
    IToastActivatedEventArgs* a = nullptr;
    if (!args || FAILED(args->QueryInterface(kIidActivatedArgs, reinterpret_cast<void**>(&a))) || !a) return false;
    HSTRING h = nullptr;
    const HRESULT hr = a->get_Arguments(&h);
    release(a);
    if (FAILED(hr) || !parse_toast_args(take_string(h), key, action)) return false;
    if (*action < 0) return true;
    IToastActivatedEventArgs2* a2 = nullptr;
    IInspectable* input = nullptr;
    IMapStringObject* map = nullptr;
    if (SUCCEEDED(args->QueryInterface(kIidActivatedArgs2, reinterpret_cast<void**>(&a2))) && a2 &&
        SUCCEEDED(a2->get_UserInput(&input)) && input &&
        SUCCEEDED(input->QueryInterface(kIidMapStringObject, reinterpret_cast<void**>(&map))) && map) {
        HStr id(toast_reply_id(static_cast<size_t>(*action)));
        IInspectable* boxed = nullptr;
        IPropertyValue* value = nullptr;
        if (SUCCEEDED(map->Lookup(id.get(), &boxed)) && boxed &&
            SUCCEEDED(boxed->QueryInterface(kIidPropertyValue, reinterpret_cast<void**>(&value))) && value) {
            HSTRING text = nullptr;
            if (SUCCEEDED(value->GetString(&text))) {
                *reply = take_string(text);
                *has_reply = true;
            }
        }
        release(value);
        release(boxed);
    }
    release(map);
    release(input);
    release(a2);
    return true;
}

// The TypedEventHandler<ToastNotification, IInspectable> of one toast's
// Activated event: delegates are IUnknown-based and are called on the thread
// pool, so the object is agile.
class ActivatedHandler final : public IUnknown {
public:
    explicit ActivatedHandler(std::wstring key) : key_(std::move(key)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualGUID(riid, kIidUnknown) || IsEqualGUID(riid, kIidAgileObject) ||
            IsEqualGUID(riid, kIidActivatedHandler)) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    // The delegate's own method, the first after IUnknown's.
    virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable* sender, IInspectable* args) {
        IToastNotification* toast = nullptr;
        if (sender) sender->QueryInterface(kIidToastNotification, reinterpret_cast<void**>(&toast));
        forget(key_, toast);  // Windows takes a clicked toast off the Action Center
        release(toast);
        std::wstring key, reply;
        int action = -1;
        bool has_reply = false;
        if (read_click(args, &key, &action, &has_reply, &reply) && action >= 0) {
            act(key_, action, has_reply ? &reply : nullptr);
        } else {
            activate(key_);
        }
        return S_OK;
    }

private:
    ~ActivatedHandler() = default;  // not virtual: no slot may follow Invoke's in the vtable
    std::wstring key_;
    LONG refs_ = 1;
};

// The factories and the notifier, once (under g_lock). False, with one line in
// the log, when Windows cannot give them: then nothing is shown, as before.
bool init_locked(JNIEnv* env) {
    if (g_state) return g_state == 1;
    g_state = 2;
    if (g_aumid.empty()) {
        std::fprintf(stderr, "[win.apk] notifications: no AppUserModelID for this process; nothing is shown\n");
        return false;
    }
    if (!g_combase.load()) {
        std::fprintf(stderr, "[win.apk] notifications: combase.dll not available; nothing is shown\n");
        return false;
    }
    if (!ro_init_thread()) return false;
    IToastNotificationManagerStatics* statics = nullptr;
    HStr manager(L"Windows.UI.Notifications.ToastNotificationManager");
    HRESULT hr = g_combase.get_factory(manager.get(), kIidManagerStatics, reinterpret_cast<void**>(&statics));
    if (FAILED(hr) || !statics) {
        log("ToastNotificationManager is not available", hr);
        return false;
    }
    HStr aumid(g_aumid);
    hr = statics->CreateToastNotifierWithId(aumid.get(), &g_notifier);
    IToastNotificationManagerStatics2* statics2 = nullptr;
    if (SUCCEEDED(statics->QueryInterface(kIidManagerStatics2, reinterpret_cast<void**>(&statics2))) && statics2) {
        if (FAILED(statics2->get_History(&g_history))) g_history = nullptr;
        release(statics2);
    }
    release(statics);
    if (FAILED(hr) || !g_notifier) {
        log("CreateToastNotifier failed (no Start menu shortcut with this id?)", hr);
        return false;
    }
    HStr toast(L"Windows.UI.Notifications.ToastNotification");
    hr = g_combase.get_factory(toast.get(), kIidToastFactory, reinterpret_cast<void**>(&g_factory));
    if (FAILED(hr) || !g_factory) {
        log("ToastNotification factory is not available", hr);
        return false;
    }
    int setting = kSettingEnabled;
    if (SUCCEEDED(g_notifier->get_Setting(&setting)) && setting != kSettingEnabled) {
        // Settings > System > Notifications: Windows will not show them, but
        // the app is told nothing (Android's switch is the permission).
        std::fprintf(stderr, "[win.apk] notifications: turned off in Windows settings (%d); posting anyway\n",
                     setting);
    }
    env->GetJavaVM(&g_vm);
    if (jclass c = env->FindClass("com/macapk/loader/HostNotifications")) {
        g_bridge = static_cast<jclass>(env->NewGlobalRef(c));
        g_on_activated = env->GetStaticMethodID(c, "onActivated", "(Ljava/lang/String;)V");
        g_on_action = env->GetStaticMethodID(c, "onAction", "(Ljava/lang/String;ILjava/lang/String;)V");
        env->DeleteLocalRef(c);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!g_on_activated || !g_on_action) {
        std::fprintf(stderr, "[win.apk] notifications: HostNotifications.onActivated or onAction not found\n");
        return false;
    }
    g_state = 1;
    return true;
}

jboolean JNICALL Notifications_init(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> hold(g_lock);
    return init_locked(env) ? JNI_TRUE : JNI_FALSE;
}

// The actions as the Java side lays them out: a title each, and a field's label for those that
// take a reply (null for the others).
std::vector<ToastAction> actions_of(JNIEnv* env, jobjectArray titles, jobjectArray replies) {
    std::vector<ToastAction> out;
    const jsize n = titles ? env->GetArrayLength(titles) : 0;
    for (jsize i = 0; i < n && out.size() < kToastMaxActions; ++i) {
        ToastAction a;
        auto t = static_cast<jstring>(env->GetObjectArrayElement(titles, i));
        a.title = wide(env, t);
        if (t) env->DeleteLocalRef(t);
        if (replies && i < env->GetArrayLength(replies)) {
            if (auto l = static_cast<jstring>(env->GetObjectArrayElement(replies, i))) {
                a.reply = true;
                a.reply_label = wide(env, l);
                env->DeleteLocalRef(l);
            }
        }
        out.push_back(std::move(a));
    }
    return out;
}

jboolean JNICALL Notifications_post(JNIEnv* env, jclass, jstring jkey, jstring jtitle, jstring jtext, jstring jsub,
                                    jbyteArray jicon, jint max, jint progress, jboolean indeterminate,
                                    jboolean popup, jobjectArray jactions, jobjectArray jreplies) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (!init_locked(env) || !ro_init_thread()) return JNI_FALSE;
    const std::wstring key = wide(env, jkey);
    const std::wstring tag = tag_of(key);
    std::wstring icon;
    if (jicon && env->GetArrayLength(jicon) > 0) {
        const std::wstring path = icon_path(tag);
        if (!path.empty()) {
            CreateDirectoryW(icons_dir().c_str(), nullptr);
            jbyte* bytes = env->GetByteArrayElements(jicon, nullptr);
            if (bytes) {
                if (write_file(path, bytes, static_cast<size_t>(env->GetArrayLength(jicon)))) icon = path;
                env->ReleaseByteArrayElements(jicon, bytes, JNI_ABORT);
            }
        }
    }
    const std::wstring xml = toast_xml(key, wide(env, jtitle), wide(env, jtext), wide(env, jsub), icon, max, progress,
                                       indeterminate == JNI_TRUE, popup == JNI_TRUE, actions_of(env, jactions, jreplies));
    IInspectable* doc = nullptr;
    HStr xml_class(L"Windows.Data.Xml.Dom.XmlDocument");
    HRESULT hr = g_combase.activate(xml_class.get(), &doc);
    if (FAILED(hr) || !doc) {
        log("XmlDocument is not available", hr);
        return JNI_FALSE;
    }
    IXmlDocumentIO* io = nullptr;
    hr = doc->QueryInterface(kIidXmlDocumentIO, reinterpret_cast<void**>(&io));
    if (SUCCEEDED(hr) && io) {
        HStr text(xml);
        hr = io->LoadXml(text.get());
        release(io);
    }
    if (FAILED(hr)) {
        log("the toast's XML was refused", hr);
        release(doc);
        return JNI_FALSE;
    }
    IToastNotification* toast = nullptr;
    hr = g_factory->CreateToastNotification(doc, &toast);
    release(doc);
    if (FAILED(hr) || !toast) {
        log("CreateToastNotification failed", hr);
        return JNI_FALSE;
    }
    IToastNotification2* t2 = nullptr;
    if (SUCCEEDED(toast->QueryInterface(kIidToastNotification2, reinterpret_cast<void**>(&t2))) && t2) {
        HStr htag(tag), hgroup(kGroup);
        t2->put_Tag(htag.get());
        t2->put_Group(hgroup.get());
        t2->put_SuppressPopup(popup == JNI_TRUE ? 0 : 1);
        release(t2);
    }
    auto* handler = new ActivatedHandler(key);
    EventToken token{};
    hr = toast->add_Activated(handler, &token);
    handler->Release();
    if (FAILED(hr)) {
        // The toast still shows; only a click does nothing. Logged once, so a
        // Windows that refuses the handler is found out.
        static bool reported = false;
        if (!reported) log("add_Activated failed; clicks will not reach the app", hr);
        reported = true;
    }
    // A key notified again: the new toast takes the old one's place (the same
    // tag and group) in the Action Center; the old object is let go.
    auto it = g_shown.find(key);
    if (it != g_shown.end()) {
        it->second->Release();
        g_shown.erase(it);
    }
    hr = g_notifier->Show(toast);
    if (FAILED(hr)) {
        log("Show failed", hr);
        release(toast);
        return JNI_FALSE;
    }
    g_shown[key] = toast;
    return JNI_TRUE;
}

void JNICALL Notifications_cancel(JNIEnv* env, jclass, jstring jkey) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (!init_locked(env) || !ro_init_thread()) return;
    const std::wstring key = wide(env, jkey);
    const std::wstring tag = tag_of(key);
    auto it = g_shown.find(key);
    if (it != g_shown.end()) {
        g_notifier->Hide(it->second);  // the banner, if it is still up
        it->second->Release();
        g_shown.erase(it);
    }
    // The Action Center, by tag: also a toast of a previous run of the app.
    if (g_history) {
        HStr htag(tag), hgroup(kGroup), app(g_aumid);
        g_history->RemoveGroupedTagWithId(htag.get(), hgroup.get(), app.get());
    }
    const std::wstring icon = icon_path(tag);
    if (!icon.empty()) DeleteFileW(icon.c_str());
}

void JNICALL Notifications_cancelAll(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (!init_locked(env) || !ro_init_thread()) return;
    for (auto& [key, toast] : g_shown) {
        g_notifier->Hide(toast);
        toast->Release();
    }
    g_shown.clear();
    if (g_history) {
        HStr app(g_aumid);
        g_history->ClearWithId(app.get());
    }
    remove_icons();
}

constexpr const char* kCls = "com.macapk.loader.HostNotifications";

const HostMethod kMethods[] = {
    WINAPK_HOST_NATIVE(kCls, "nativeInit", "()Z", Notifications_init),
    WINAPK_HOST_NATIVE(kCls, "nativePost",
                       "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[BIIZZ[Ljava/lang/String;[Ljava/lang/String;)Z",
                       Notifications_post),
    WINAPK_HOST_NATIVE(kCls, "nativeCancel", "(Ljava/lang/String;)V", Notifications_cancel),
    WINAPK_HOST_NATIVE(kCls, "nativeCancelAll", "()V", Notifications_cancelAll),
};

}  // namespace

void set_app_user_model_id(const std::wstring& app_name) {
    const std::wstring id = app_user_model_id(app_name);
    {
        std::lock_guard<std::mutex> hold(g_lock);
        g_aumid = id;
    }
    const HRESULT hr = SetCurrentProcessExplicitAppUserModelID(id.c_str());
    if (FAILED(hr)) log("SetCurrentProcessExplicitAppUserModelID failed", hr);
}

HostMethods native_notifications_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

}  // namespace winapk::host
