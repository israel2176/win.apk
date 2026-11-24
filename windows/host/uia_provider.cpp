// uia_provider.cpp -- the app window's views for Windows' UI Automation (uia_provider.h).
//
// Adapted from the other runtime (https://example.invalid/other-runtime), compat/accessibility/uia_bridge.cc,
// Copyright (C) 2026 its authors, SPDX-License-Identifier: Apache-2.0. Modified for Win.apk:
// the providers answer from a snapshot the app publishes instead of calling Java on UIA's threads
// (no thread of UIA's is ever attached to the VM or waits for the app's main thread), the window
// is Win.apk's one app window instead of subclassed ones, UIA's API is loaded only when a client
// first asks, and range values, scrolling, scroll-into-view, the app's language (Culture) and
// live regions are added.
#include "uia_provider.h"

#include <ole2.h>
#include <oleauto.h>
#include <uiautomation.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace winapk::host::uia {

namespace {

constexpr double kNoScroll = -1.0;  // UIA_ScrollPatternNoScroll
constexpr ULONGLONG kIdleMs = 60000;  // no client call for this long, and no listener: off
constexpr DWORD kFirstSnapshotMs = 1500;  // how long a first call waits for the app's first snapshot

HRESULT hr(unsigned long code) { return static_cast<HRESULT>(code); }

// ---- UIA's API, loaded when a client first asks ---------------------------------------------

struct Api {
    LRESULT(WINAPI* return_provider)(HWND, WPARAM, LPARAM, IRawElementProviderSimple*) = nullptr;
    HRESULT(WINAPI* host_from_hwnd)(HWND, IRawElementProviderSimple**) = nullptr;
    HRESULT(WINAPI* raise_event)(IRawElementProviderSimple*, EVENTID) = nullptr;
    HRESULT(WINAPI* raise_property)(IRawElementProviderSimple*, PROPERTYID, VARIANT, VARIANT) = nullptr;
    HRESULT(WINAPI* raise_structure)(IRawElementProviderSimple*, StructureChangeType, int*, int) = nullptr;
    HRESULT(WINAPI* raise_notification)(IRawElementProviderSimple*, NotificationKind, NotificationProcessing, BSTR,
                                        BSTR) = nullptr;  // Windows 10 1709 and later
    BOOL(WINAPI* clients_listening)() = nullptr;
    bool ok = false;
};

const Api& api() {
    static const Api a = [] {
        Api r;
        HMODULE m = LoadLibraryW(L"uiautomationcore.dll");
        if (!m) return r;
        auto get = [m](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(
                reinterpret_cast<void*>(GetProcAddress(m, name)));
        };
        get(r.return_provider, "UiaReturnRawElementProvider");
        get(r.host_from_hwnd, "UiaHostProviderFromHwnd");
        get(r.raise_event, "UiaRaiseAutomationEvent");
        get(r.raise_property, "UiaRaiseAutomationPropertyChangedEvent");
        get(r.raise_structure, "UiaRaiseStructureChangedEvent");
        get(r.raise_notification, "UiaRaiseNotificationEvent");
        get(r.clients_listening, "UiaClientsAreListening");
        r.ok = r.return_provider && r.host_from_hwnd;
        return r;
    }();
    return a;
}

bool clients_listening() {
    const Api& a = api();
    return a.clients_listening && a.clients_listening();
}

// ---- One worker: the app's side and the events ------------------------------------------------
//
// Calls into the app and raised events leave UIA's threads, the window's and the app's main
// thread at once: a client answering an event can call the providers back.

class Worker {
public:
    void post(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> hold(lock_);
            if (!started_) {
                started_ = true;
                std::thread([this] { run(); }).detach();
            }
            jobs_.push_back(std::move(job));
        }
        wake_.notify_one();
    }

private:
    void run() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> hold(lock_);
                wake_.wait(hold, [this] { return !jobs_.empty(); });
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }

    std::mutex lock_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    bool started_ = false;
};

Worker& worker() {
    static Worker* w = new Worker();  // the process's: never destroyed under its thread
    return *w;
}

// ---- State ----------------------------------------------------------------------------------

std::mutex g_lock;  // g_hwnd, g_geometry, g_snapshot
std::condition_variable g_published;
HWND g_hwnd = nullptr;
DWORD g_window_thread = 0;
Geometry g_geometry;
std::shared_ptr<const Snapshot> g_snapshot;
uint64_t g_generation = 0;

std::atomic<AppSide*> g_app{nullptr};
std::atomic<bool> g_active{false};
std::atomic<ULONGLONG> g_last_use{0};
std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_frames_published{~0ull};
std::atomic<bool> g_refresh{false};

void request_refresh() {
    if (g_refresh.exchange(true)) return;
    worker().post([] {
        if (AppSide* app = g_app.load()) app->refresh();
    });
}

// A client called: the app is told on the first call after a quiet spell.
void touch() {
    g_last_use = GetTickCount64();
    if (!g_active.exchange(true)) {
        g_frames_published = ~0ull;
        worker().post([] {
            if (AppSide* app = g_app.load()) app->activate(true);
        });
    }
}

std::shared_ptr<const Snapshot> snapshot() {
    touch();
    std::unique_lock<std::mutex> hold(g_lock);
    if (g_generation == 0 && g_app.load() && GetCurrentThreadId() != g_window_thread) {
        hold.unlock();
        request_refresh();
        hold.lock();
        g_published.wait_for(hold, std::chrono::milliseconds(kFirstSnapshotMs), [] { return g_generation != 0; });
    }
    return g_snapshot;
}

bool window_alive(HWND hwnd) {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_hwnd == hwnd && hwnd != nullptr;
}

BSTR bstr(const std::wstring& s) { return SysAllocStringLen(s.data(), static_cast<UINT>(s.size())); }

std::vector<std::wstring> split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t i = 0;
    for (;;) {
        const size_t j = s.find(sep, i);
        out.push_back(s.substr(i, j == std::wstring::npos ? std::wstring::npos : j - i));
        if (j == std::wstring::npos) break;
        i = j + 1;
    }
    return out;
}

int64_t to_id(const std::wstring& s) { return s.empty() ? 0 : _wcstoi64(s.c_str(), nullptr, 10); }

std::vector<int64_t> to_ids(const std::wstring& s) {
    std::vector<int64_t> out;
    if (s.empty()) return out;
    for (const std::wstring& part : split(s, L',')) out.push_back(to_id(part));
    return out;
}

bool ends_with(const std::wstring& s, const wchar_t* suffix) {
    const size_t k = wcslen(suffix);
    return s.size() >= k && s.compare(s.size() - k, k, suffix) == 0;
}

}  // namespace

// ---- The snapshot ---------------------------------------------------------------------------

int control_type(const Node& n) {
    const std::wstring& c = n.class_name;
    if (ends_with(c, L"EditText") || (n.flags & kEditable)) return UIA_EditControlTypeId;
    if (ends_with(c, L"RadioButton")) return UIA_RadioButtonControlTypeId;
    if (ends_with(c, L"CheckBox") || ends_with(c, L"Switch") || ends_with(c, L"ToggleButton") ||
        ends_with(c, L"SwitchCompat") || ends_with(c, L"SwitchMaterial")) {
        return UIA_CheckBoxControlTypeId;
    }
    if (ends_with(c, L"Button") || ends_with(c, L"ImageButton")) return UIA_ButtonControlTypeId;
    if (ends_with(c, L"SeekBar") || ((n.flags & kRange) && (n.can & kCanSetProgress))) return UIA_SliderControlTypeId;
    if (ends_with(c, L"ProgressBar") || (n.flags & kRange)) return UIA_ProgressBarControlTypeId;
    if (ends_with(c, L"Spinner")) return UIA_ComboBoxControlTypeId;
    if (ends_with(c, L"ListView") || ends_with(c, L"RecyclerView") || ends_with(c, L"GridView") ||
        (n.flags & kCollection)) {
        return UIA_ListControlTypeId;
    }
    if (n.flags & kCollectionItem) return UIA_ListItemControlTypeId;
    if (ends_with(c, L"ImageView")) return (n.flags & kClickable) ? UIA_ButtonControlTypeId : UIA_ImageControlTypeId;
    if (ends_with(c, L"ScrollView") || (n.flags & kScrollable)) return UIA_PaneControlTypeId;
    if (ends_with(c, L"TextView")) return (n.flags & kClickable) ? UIA_ButtonControlTypeId : UIA_TextControlTypeId;
    if (n.flags & kCheckable) return UIA_CheckBoxControlTypeId;
    if (n.flags & kClickable) return UIA_ButtonControlTypeId;
    if (!n.children.empty()) return UIA_GroupControlTypeId;
    return n.name.empty() ? UIA_CustomControlTypeId : UIA_TextControlTypeId;
}

bool parse_snapshot(const std::wstring& text, Snapshot* out) {
    std::vector<std::wstring> lines = split(text, L'\n');
    if (lines.empty()) return false;
    std::vector<std::wstring> head = split(lines[0], L'\t');
    if (head.size() < 4 || head[0] != L"W1") return false;
    Snapshot s;
    s.focused = to_id(head[1]);
    s.locale = head[2];
    s.lcid = s.locale.empty() ? 0 : LocaleNameToLCID(s.locale.c_str(), 0);
    s.top = to_ids(head[3]);
    for (size_t i = 1; i < lines.size(); ++i) {
        if (lines[i].empty()) continue;
        std::vector<std::wstring> f = split(lines[i], L'\t');
        if (f.size() < 13) return false;
        Node n;
        n.id = to_id(f[0]);
        n.parent = to_id(f[1]);
        n.class_name = f[2];
        n.name = f[3];
        n.value = f[4];
        n.help = f[5];
        n.flags = static_cast<uint32_t>(wcstoul(f[6].c_str(), nullptr, 10));
        std::vector<std::wstring> b = split(f[7], L',');
        if (b.size() == 4) {
            n.bounds = RECT{static_cast<LONG>(to_id(b[0])), static_cast<LONG>(to_id(b[1])),
                            static_cast<LONG>(to_id(b[2])), static_cast<LONG>(to_id(b[3]))};
        }
        n.children = to_ids(f[8]);
        n.automation_id = f[9];
        n.can = static_cast<uint32_t>(wcstoul(f[10].c_str(), nullptr, 10));
        std::vector<std::wstring> r = split(f[11], L',');
        if (r.size() == 3) {
            n.range_min = wcstod(r[0].c_str(), nullptr);
            n.range_max = wcstod(r[1].c_str(), nullptr);
            n.range_now = wcstod(r[2].c_str(), nullptr);
        }
        n.live = static_cast<int>(to_id(f[12]));
        n.control_type = control_type(n);
        if (n.id == 0 || s.index.count(n.id)) continue;  // 0 is the root's; a repeat is the app's mistake
        s.index[n.id] = s.nodes.size();
        s.nodes.push_back(std::move(n));
    }
    *out = std::move(s);
    return true;
}

namespace {

// ---- The providers --------------------------------------------------------------------------
//
// One class serves every element; id 0 is the window's root (the fragment root, which UIA
// merges with the window's own HWND provider), every other id a node of the snapshot.

constexpr int64_t kRoot = 0;

class Element final : public IRawElementProviderSimple,
                      public IRawElementProviderFragment,
                      public IRawElementProviderFragmentRoot,
                      public IInvokeProvider,
                      public IValueProvider,
                      public IToggleProvider,
                      public IRangeValueProvider,
                      public IScrollProvider,
                      public IScrollItemProvider {
public:
    Element(HWND hwnd, int64_t id) : hwnd_(hwnd), id_(id) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IRawElementProviderSimple))) {
            *out = static_cast<IRawElementProviderSimple*>(this);
        } else if (IsEqualIID(riid, __uuidof(IRawElementProviderFragment))) {
            *out = static_cast<IRawElementProviderFragment*>(this);
        } else if (IsEqualIID(riid, __uuidof(IRawElementProviderFragmentRoot)) && id_ == kRoot) {
            *out = static_cast<IRawElementProviderFragmentRoot*>(this);
        } else if (IsEqualIID(riid, __uuidof(IInvokeProvider))) {
            *out = static_cast<IInvokeProvider*>(this);
        } else if (IsEqualIID(riid, __uuidof(IValueProvider))) {
            *out = static_cast<IValueProvider*>(this);
        } else if (IsEqualIID(riid, __uuidof(IToggleProvider))) {
            *out = static_cast<IToggleProvider*>(this);
        } else if (IsEqualIID(riid, __uuidof(IRangeValueProvider))) {
            *out = static_cast<IRangeValueProvider*>(this);
        } else if (IsEqualIID(riid, __uuidof(IScrollProvider))) {
            *out = static_cast<IScrollProvider*>(this);
        } else if (IsEqualIID(riid, __uuidof(IScrollItemProvider))) {
            *out = static_cast<IScrollItemProvider*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    // IRawElementProviderSimple
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* out) override {
        if (!out) return E_POINTER;
        *out = ProviderOptions_ServerSideProvider;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id_ == kRoot) return alive() ? S_OK : hr(UIA_E_ELEMENTNOTAVAILABLE);
        Node n;
        if (!get(&n)) return hr(UIA_E_ELEMENTNOTAVAILABLE);
        IUnknown* p = nullptr;
        switch (pattern) {
            case UIA_InvokePatternId:
                if ((n.flags & (kClickable | kLongClickable)) && !(n.flags & kCheckable) &&
                    n.control_type != UIA_EditControlTypeId) {
                    p = static_cast<IInvokeProvider*>(this);
                }
                break;
            case UIA_ValuePatternId:
                if (n.flags & kEditable) p = static_cast<IValueProvider*>(this);
                break;
            case UIA_TogglePatternId:
                if (n.flags & kCheckable) p = static_cast<IToggleProvider*>(this);
                break;
            case UIA_RangeValuePatternId:
                if (n.flags & kRange) p = static_cast<IRangeValueProvider*>(this);
                break;
            case UIA_ScrollPatternId:
                if (n.flags & kScrollable) p = static_cast<IScrollProvider*>(this);
                break;
            case UIA_ScrollItemPatternId:
                p = static_cast<IScrollItemProvider*>(this);
                break;
            default:
                break;
        }
        if (p) {
            p->AddRef();
            *out = p;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* out) override {
        if (!out) return E_POINTER;
        VariantInit(out);
        auto snap = snapshot();
        if (!alive()) return hr(UIA_E_ELEMENTNOTAVAILABLE);
        auto str = [out](const std::wstring& s) {
            out->vt = VT_BSTR;
            out->bstrVal = bstr(s);
        };
        auto boolean = [out](bool b) {
            out->vt = VT_BOOL;
            out->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
        };
        auto i4 = [out](LONG v) {
            out->vt = VT_I4;
            out->lVal = v;
        };
        if (property == UIA_CulturePropertyId) {
            if (snap && snap->lcid != 0) i4(static_cast<LONG>(snap->lcid));
            return S_OK;
        }
        if (property == UIA_FrameworkIdPropertyId) {
            str(L"Android");
            return S_OK;
        }
        if (property == UIA_ProcessIdPropertyId) {
            i4(static_cast<LONG>(GetCurrentProcessId()));
            return S_OK;
        }
        if (id_ == kRoot) return S_OK;  // the rest is the window's own (its HWND provider)
        const Node* found = snap ? snap->find(id_) : nullptr;
        if (!found) return hr(UIA_E_ELEMENTNOTAVAILABLE);
        const Node& n = *found;
        switch (property) {
            case UIA_NamePropertyId:
                if (!n.name.empty()) str(n.name);
                break;
            case UIA_ControlTypePropertyId:
                i4(n.control_type);
                break;
            case UIA_ClassNamePropertyId:
                str(n.class_name);
                break;
            case UIA_AutomationIdPropertyId:
                if (!n.automation_id.empty()) str(n.automation_id);
                break;
            case UIA_HelpTextPropertyId:
                if (!n.help.empty()) str(n.help);
                break;
            case UIA_IsEnabledPropertyId:
                boolean((n.flags & kEnabled) != 0);
                break;
            case UIA_IsKeyboardFocusablePropertyId:
                boolean((n.flags & kFocusable) != 0);
                break;
            case UIA_HasKeyboardFocusPropertyId:
                boolean((n.flags & kFocused) != 0);
                break;
            case UIA_IsPasswordPropertyId:
                boolean((n.flags & kPassword) != 0);
                break;
            case UIA_IsOffscreenPropertyId:
                boolean((n.flags & kVisible) == 0);
                break;
            case UIA_IsControlElementPropertyId:
            case UIA_IsContentElementPropertyId:
                boolean(true);
                break;
            case UIA_ValueValuePropertyId:
                if (n.flags & kEditable) str(n.value);
                break;
            case UIA_ValueIsReadOnlyPropertyId:
                if (n.flags & kEditable) boolean(!(n.flags & kEnabled) || !(n.can & kCanSetText));
                break;
            case UIA_ToggleToggleStatePropertyId:
                if (n.flags & kCheckable) i4((n.flags & kChecked) ? ToggleState_On : ToggleState_Off);
                break;
            case UIA_RangeValueValuePropertyId:
                if (n.flags & kRange) {
                    out->vt = VT_R8;
                    out->dblVal = n.range_now;
                }
                break;
            case UIA_HeadingLevelPropertyId:
                if (n.flags & kHeading) i4(80052);  // HeadingLevel2: Android has no level
                break;
            case UIA_LiveSettingPropertyId:
                if (n.live > 0) i4(n.live);  // Polite 1, Assertive 2, as Android's live regions
                break;
            case UIA_SelectionItemIsSelectedPropertyId:
                if (n.flags & kCollectionItem) boolean((n.flags & kSelected) != 0);
                break;
            default:
                break;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!alive()) return hr(UIA_E_ELEMENTNOTAVAILABLE);
        if (id_ == kRoot) return api().host_from_hwnd(hwnd_, out);
        return S_OK;
    }

    // IRawElementProviderFragment
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        auto snap = snapshot();
        if (!alive()) return hr(UIA_E_ELEMENTNOTAVAILABLE);
        if (!snap) return S_OK;
        const Node* n = id_ == kRoot ? nullptr : snap->find(id_);
        if (id_ != kRoot && !n) return hr(UIA_E_ELEMENTNOTAVAILABLE);
        const std::vector<int64_t>& kids = n ? n->children : snap->top;
        int64_t target = -1;
        switch (direction) {
            case NavigateDirection_Parent:
                if (n) target = snap->find(n->parent) ? n->parent : kRoot;
                break;
            case NavigateDirection_FirstChild:
                if (!kids.empty()) target = kids.front();
                break;
            case NavigateDirection_LastChild:
                if (!kids.empty()) target = kids.back();
                break;
            case NavigateDirection_NextSibling:
            case NavigateDirection_PreviousSibling: {
                if (!n) break;
                const Node* p = snap->find(n->parent);
                const std::vector<int64_t>& sibs = p ? p->children : snap->top;
                for (size_t i = 0; i < sibs.size(); ++i) {
                    if (sibs[i] != id_) continue;
                    if (direction == NavigateDirection_NextSibling && i + 1 < sibs.size()) target = sibs[i + 1];
                    if (direction == NavigateDirection_PreviousSibling && i > 0) target = sibs[i - 1];
                    break;
                }
                break;
            }
        }
        if (target < 0) return S_OK;
        *out = new Element(hwnd_, target);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id_ == kRoot) return S_OK;  // the window's
        int ids[3] = {UiaAppendRuntimeId, static_cast<int>(static_cast<uint64_t>(id_) >> 32),
                      static_cast<int>(id_ & 0xffffffff)};
        SAFEARRAY* sa = SafeArrayCreateVector(VT_I4, 0, 3);
        if (!sa) return E_OUTOFMEMORY;
        for (LONG i = 0; i < 3; i++) SafeArrayPutElement(sa, &i, &ids[i]);
        *out = sa;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* out) override {
        if (!out) return E_POINTER;
        *out = UiaRect{0, 0, 0, 0};
        auto snap = snapshot();
        Geometry geo;
        {
            std::lock_guard<std::mutex> hold(g_lock);
            if (g_hwnd != hwnd_ || !hwnd_) return hr(UIA_E_ELEMENTNOTAVAILABLE);
            geo = g_geometry;
        }
        if (id_ == kRoot) return S_OK;  // the window's own
        const Node* n = snap ? snap->find(id_) : nullptr;
        if (!n) return hr(UIA_E_ELEMENTNOTAVAILABLE);
        RECT r{};
        if (geo.to_screen && geo.to_screen(n->bounds, &r)) {
            out->left = r.left;
            out->top = r.top;
            out->width = r.right - r.left;
            out->height = r.bottom - r.top;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** out) override {
        if (out) *out = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetFocus() override { return act(kActFocus, L"", 0); }

    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** out) override {
        if (!out) return E_POINTER;
        *out = new Element(hwnd_, kRoot);
        return S_OK;
    }

    // IRawElementProviderFragmentRoot (the root only): the topmost window first, the deepest
    // node there, a later child over an earlier one (drawn later).
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        auto snap = snapshot();
        Geometry geo;
        {
            std::lock_guard<std::mutex> hold(g_lock);
            geo = g_geometry;
        }
        POINT p{};
        int64_t best = kRoot;
        if (snap && geo.to_frame && geo.to_frame(POINT{static_cast<LONG>(x), static_cast<LONG>(y)}, &p)) {
            const std::vector<int64_t>* level = &snap->top;
            for (int depth = 0; depth < 256 && level; ++depth) {
                const Node* hit = nullptr;
                for (auto it = level->rbegin(); it != level->rend(); ++it) {
                    const Node* c = snap->find(*it);
                    if (c && (c->flags & kVisible) && PtInRect(&c->bounds, p)) {
                        hit = c;
                        break;
                    }
                }
                if (!hit) break;
                best = hit->id;
                level = &hit->children;
            }
        }
        *out = new Element(hwnd_, best);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        auto snap = snapshot();
        if (snap && snap->focused != 0 && snap->find(snap->focused)) *out = new Element(hwnd_, snap->focused);
        return S_OK;
    }

    // IInvokeProvider
    HRESULT STDMETHODCALLTYPE Invoke() override { return act(kActClick, L"", kEnabled); }

    // IValueProvider
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override {
        Node n;
        if (!get(&n)) return hr(UIA_E_ELEMENTNOTAVAILABLE);
        if (!(n.flags & kEditable) || !(n.can & kCanSetText)) return hr(UIA_E_INVALIDOPERATION);
        return act(kActSetText, value ? value : L"", kEnabled);
    }
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* out) override {
        if (!out) return E_POINTER;
        Node n;
        if (!get(&n)) return hr(UIA_E_ELEMENTNOTAVAILABLE);
        *out = bstr(n.value);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* out) override {
        if (!out) return E_POINTER;
        Node n;
        if (!get(&n)) return hr(UIA_E_ELEMENTNOTAVAILABLE);
        *out = (n.flags & kRange) ? !(n.can & kCanSetProgress) : (!(n.flags & kEnabled) || !(n.can & kCanSetText));
        return S_OK;
    }

    // IToggleProvider
    HRESULT STDMETHODCALLTYPE Toggle() override { return act(kActClick, L"", kEnabled); }
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* out) override {
        if (!out) return E_POINTER;
        Node n;
        if (!get(&n)) return hr(UIA_E_ELEMENTNOTAVAILABLE);
        *out = (n.flags & kChecked) ? ToggleState_On : ToggleState_Off;
        return S_OK;
    }

    // IRangeValueProvider (a ProgressBar's or a SeekBar's RangeInfo; ACTION_SET_PROGRESS)
    HRESULT STDMETHODCALLTYPE SetValue(double value) override {
        Node n;
        if (!get(&n)) return hr(UIA_E_ELEMENTNOTAVAILABLE);
        if (!(n.can & kCanSetProgress)) return hr(UIA_E_INVALIDOPERATION);
        if (value < n.range_min || value > n.range_max) return E_INVALIDARG;
        wchar_t text[64];
        swprintf(text, 64, L"%.17g", value);
        return act(kActSetProgress, text, kEnabled);
    }
    HRESULT STDMETHODCALLTYPE get_Value(double* out) override { return range(out, [](const Node& n) { return n.range_now; }); }
    HRESULT STDMETHODCALLTYPE get_Maximum(double* out) override { return range(out, [](const Node& n) { return n.range_max; }); }
    HRESULT STDMETHODCALLTYPE get_Minimum(double* out) override { return range(out, [](const Node& n) { return n.range_min; }); }
    // AbsSeekBar's default key step: a twentieth of the range.
    HRESULT STDMETHODCALLTYPE get_LargeChange(double* out) override {
        return range(out, [](const Node& n) { return (n.range_max - n.range_min) / 20; });
    }
    HRESULT STDMETHODCALLTYPE get_SmallChange(double* out) override {
        return range(out, [](const Node& n) { return (n.range_max - n.range_min) / 20; });
    }

    // IScrollProvider: Android scrolls by a page (ACTION_SCROLL_FORWARD / BACKWARD, or the
    // directional ones a view offers); it tells no position, so the percentages say only
    // whether either end is reached.
    HRESULT STDMETHODCALLTYPE Scroll(ScrollAmount horizontal, ScrollAmount vertical) override {
        Node n;
        if (!get(&n)) return hr(UIA_E_ELEMENTNOTAVAILABLE);
        auto forward = [](ScrollAmount a) { return a == ScrollAmount_LargeIncrement || a == ScrollAmount_SmallIncrement; };
        auto backward = [](ScrollAmount a) { return a == ScrollAmount_LargeDecrement || a == ScrollAmount_SmallDecrement; };
        int action = 0;
        if (forward(vertical)) action = (n.can & kCanScrollDown) ? kActScrollDown : kActScrollForward;
        else if (backward(vertical)) action = (n.can & kCanScrollUp) ? kActScrollUp : kActScrollBackward;
        else if (forward(horizontal)) action = (n.can & kCanScrollRight) ? kActScrollRight : kActScrollForward;
        else if (backward(horizontal)) action = (n.can & kCanScrollLeft) ? kActScrollLeft : kActScrollBackward;
        if (action == 0) return S_OK;
        return act(action, L"", 0);
    }
    HRESULT STDMETHODCALLTYPE SetScrollPercent(double horizontal, double vertical) override {
        if (horizontal == kNoScroll && vertical == kNoScroll) return S_OK;
        return hr(UIA_E_INVALIDOPERATION);
    }
    HRESULT STDMETHODCALLTYPE get_HorizontalScrollPercent(double* out) override { return percent(out, true); }
    HRESULT STDMETHODCALLTYPE get_VerticalScrollPercent(double* out) override { return percent(out, false); }
    HRESULT STDMETHODCALLTYPE get_HorizontalViewSize(double* out) override { return view_size(out, true); }
    HRESULT STDMETHODCALLTYPE get_VerticalViewSize(double* out) override { return view_size(out, false); }
    HRESULT STDMETHODCALLTYPE get_HorizontallyScrollable(BOOL* out) override {
        if (!out) return E_POINTER;
        Node n;
        if (!get(&n)) return hr(UIA_E_ELEMENTNOTAVAILABLE);
        *out = horizontal(n) && (n.flags & kScrollable);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_VerticallyScrollable(BOOL* out) override {
        if (!out) return E_POINTER;
        Node n;
        if (!get(&n)) return hr(UIA_E_ELEMENTNOTAVAILABLE);
        *out = !horizontal(n) && (n.flags & kScrollable);
        return S_OK;
    }

    // IScrollItemProvider
    HRESULT STDMETHODCALLTYPE ScrollIntoView() override { return act(kActShowOnScreen, L"", 0); }

private:
    ~Element() = default;

    bool alive() const { return window_alive(hwnd_); }

    bool get(Node* out) {
        auto snap = snapshot();
        if (!alive() || !snap || id_ == kRoot) return false;
        const Node* n = snap->find(id_);
        if (!n) return false;
        *out = *n;
        return true;
    }

    // An action for the app's main thread; UIA's patterns are asynchronous on this side too.
    HRESULT act(int action, const std::wstring& text, uint32_t need) {
        Node n;
        if (!get(&n)) return hr(UIA_E_ELEMENTNOTAVAILABLE);
        if ((n.flags & need) != need) return hr(UIA_E_ELEMENTNOTENABLED);
        const int64_t id = id_;
        worker().post([id, action, text] {
            if (AppSide* app = g_app.load()) app->perform(id, action, text);
        });
        return S_OK;
    }

    template <typename F>
    HRESULT range(double* out, F field) {
        if (!out) return E_POINTER;
        Node n;
        if (!get(&n)) return hr(UIA_E_ELEMENTNOTAVAILABLE);
        *out = (n.flags & kRange) ? field(n) : 0.0;
        return S_OK;
    }

    static bool horizontal(const Node& n) {
        if (n.can & (kCanScrollLeft | kCanScrollRight)) return true;
        if (n.can & (kCanScrollUp | kCanScrollDown)) return false;
        return n.class_name.find(L"Horizontal") != std::wstring::npos;
    }

    HRESULT percent(double* out, bool across) {
        if (!out) return E_POINTER;
        Node n;
        if (!get(&n)) return hr(UIA_E_ELEMENTNOTAVAILABLE);
        if (!(n.flags & kScrollable) || horizontal(n) != across) {
            *out = kNoScroll;
            return S_OK;
        }
        const bool back = n.can & (kCanScrollBackward | kCanScrollUp | kCanScrollLeft);
        const bool fwd = n.can & (kCanScrollForward | kCanScrollDown | kCanScrollRight);
        *out = !back ? 0.0 : !fwd ? 100.0 : 50.0;
        return S_OK;
    }

    HRESULT view_size(double* out, bool across) {
        if (!out) return E_POINTER;
        Node n;
        if (!get(&n)) return hr(UIA_E_ELEMENTNOTAVAILABLE);
        *out = (n.flags & kScrollable) && horizontal(n) == across ? 50.0 : 100.0;
        return S_OK;
    }

    std::atomic<ULONG> refs_{1};
    HWND hwnd_;
    int64_t id_;
};

// ---- Events -----------------------------------------------------------------------------------

void raise_event(HWND hwnd, int64_t id, EVENTID event) {
    const Api& a = api();
    if (!a.raise_event) return;
    Element* e = new Element(hwnd, id);
    a.raise_event(e, event);
    e->Release();
}

void raise_property(HWND hwnd, int64_t id, PROPERTYID property, VARIANT after) {
    const Api& a = api();
    if (!a.raise_property) return;
    VARIANT before;
    VariantInit(&before);
    Element* e = new Element(hwnd, id);
    a.raise_property(e, property, before, after);
    e->Release();
}

uint64_t structure_hash(const Snapshot& s) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](int64_t v) {
        h ^= static_cast<uint64_t>(v);
        h *= 1099511628211ull;
    };
    for (int64_t t : s.top) mix(t);
    for (const Node& n : s.nodes) {
        mix(n.id);
        mix(n.parent);
        for (int64_t c : n.children) mix(c);
        mix(-1);
    }
    return h;
}

// What changed between two snapshots, as UIA events, raised on the worker.
void raise_changes(HWND hwnd, std::shared_ptr<const Snapshot> before, std::shared_ptr<const Snapshot> now) {
    if (!clients_listening()) return;
    const Api& a = api();
    if (!before || structure_hash(*before) != structure_hash(*now)) {
        if (a.raise_structure) {
            Element* root = new Element(hwnd, kRoot);
            int ids[3] = {UiaAppendRuntimeId, 0, 0};
            a.raise_structure(root, StructureChangeType_ChildrenInvalidated, ids, 3);
            root->Release();
        }
    }
    if (!before) return;
    if (now->focused != 0 && now->focused != before->focused) raise_event(hwnd, now->focused, UIA_AutomationFocusChangedEventId);
    int raised = 0;
    for (const Node& n : now->nodes) {
        if (raised >= 64) break;
        const Node* o = before->find(n.id);
        if (!o) continue;
        VARIANT v;
        VariantInit(&v);
        if ((n.flags & kEditable) && n.value != o->value) {
            v.vt = VT_BSTR;
            v.bstrVal = bstr(n.value);
            raise_property(hwnd, n.id, UIA_ValueValuePropertyId, v);
            VariantClear(&v);
            ++raised;
        }
        if ((n.flags & kCheckable) && (n.flags & kChecked) != (o->flags & kChecked)) {
            v.vt = VT_I4;
            v.lVal = (n.flags & kChecked) ? ToggleState_On : ToggleState_Off;
            raise_property(hwnd, n.id, UIA_ToggleToggleStatePropertyId, v);
            ++raised;
        }
        if ((n.flags & kRange) && n.range_now != o->range_now) {
            v.vt = VT_R8;
            v.dblVal = n.range_now;
            raise_property(hwnd, n.id, UIA_RangeValueValuePropertyId, v);
            ++raised;
        }
        if (n.name != o->name) {
            v.vt = VT_BSTR;
            v.bstrVal = bstr(n.name);
            raise_property(hwnd, n.id, UIA_NamePropertyId, v);
            VariantClear(&v);
            ++raised;
            // A live region's new text is read out, as TalkBack reads it.
            if (n.live > 0) raise_event(hwnd, n.id, UIA_LiveRegionChangedEventId);
        }
    }
}

}  // namespace

// ---- The window's thread ----------------------------------------------------------------------

void attach(HWND hwnd, const Geometry& geometry) {
    std::lock_guard<std::mutex> hold(g_lock);
    g_hwnd = hwnd;
    g_window_thread = GetCurrentThreadId();
    g_geometry = geometry;
}

bool on_get_object(HWND hwnd, WPARAM wp, LPARAM lp, LRESULT* result) {
    if (static_cast<LONG>(lp) != UiaRootObjectId || !window_alive(hwnd)) return false;
    const Api& a = api();
    if (!a.ok) return false;
    touch();
    Element* root = new Element(hwnd, kRoot);
    *result = a.return_provider(hwnd, wp, lp, root);
    root->Release();
    return true;
}

void detach(HWND hwnd) {
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (g_hwnd != hwnd) return;
        g_hwnd = nullptr;
    }
    // Tells UIA the window's providers are gone (only if it was ever asked: the DLL is loaded).
    if (GetModuleHandleW(L"uiautomationcore.dll")) {
        const Api& a = api();
        if (a.return_provider) a.return_provider(hwnd, 0, 0, nullptr);
    }
}

// ---- Any thread ------------------------------------------------------------------------------

void note_frame() { g_frames.fetch_add(1, std::memory_order_relaxed); }

void set_app(AppSide* app) { g_app = app; }

bool active() { return g_active.load(); }

bool due() {
    if (!g_active.load()) return false;
    if (g_refresh.load()) return true;
    return g_frames.load(std::memory_order_relaxed) != g_frames_published.load();
}

bool publish(const std::wstring& text) {
    g_refresh = false;
    g_frames_published = g_frames.load(std::memory_order_relaxed);
    auto next = std::make_shared<Snapshot>();
    std::shared_ptr<const Snapshot> before;
    HWND hwnd = nullptr;
    if (parse_snapshot(text, next.get())) {
        std::lock_guard<std::mutex> hold(g_lock);
        before = g_snapshot;
        next->generation = ++g_generation;
        g_snapshot = next;
        hwnd = g_hwnd;
    }
    g_published.notify_all();
    if (hwnd) {
        std::shared_ptr<const Snapshot> now = next;
        worker().post([hwnd, before, now] {
            if (window_alive(hwnd)) raise_changes(hwnd, before, now);
        });
    }
    const bool keep = GetTickCount64() - g_last_use.load() < kIdleMs || clients_listening();
    if (!keep) g_active = false;
    return keep;
}

void announce(const std::wstring& text, bool assertive) {
    if (!g_active.load() || text.empty()) return;
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        hwnd = g_hwnd;
    }
    if (!hwnd) return;
    worker().post([hwnd, text, assertive] {
        const Api& a = api();
        if (!a.raise_notification || !clients_listening() || !window_alive(hwnd)) return;
        Element* root = new Element(hwnd, kRoot);
        BSTR t = bstr(text), id = bstr(L"android.announcement");
        a.raise_notification(root, NotificationKind_Other,
                             assertive ? NotificationProcessing_ImportantMostRecent : NotificationProcessing_MostRecent,
                             t, id);
        SysFreeString(t);
        SysFreeString(id);
        root->Release();
    });
}

IUnknown* provider_for(HWND hwnd, int64_t id) {
    return static_cast<IRawElementProviderSimple*>(new Element(hwnd, id));
}

}  // namespace winapk::host::uia
