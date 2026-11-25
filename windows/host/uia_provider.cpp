// uia_provider.cpp -- the app window's views as UI Automation providers. See uia_provider.h.
//
// Written from Microsoft's UI Automation provider documentation (server-side providers:
// IRawElementProviderSimple, -Fragment, -FragmentRoot, -AdviseEvents; the control patterns; the
// Uia* event functions) over the model of uia_tree.h.
#include "uia_provider.h"

#include <ole2.h>
#include <oleauto.h>
#include <uiautomation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace winapk::host::uia {
namespace {

static_assert(control::kButton == UIA_ButtonControlTypeId && control::kCheckBox == UIA_CheckBoxControlTypeId &&
              control::kComboBox == UIA_ComboBoxControlTypeId && control::kEdit == UIA_EditControlTypeId &&
              control::kImage == UIA_ImageControlTypeId && control::kListItem == UIA_ListItemControlTypeId &&
              control::kList == UIA_ListControlTypeId && control::kProgressBar == UIA_ProgressBarControlTypeId &&
              control::kRadioButton == UIA_RadioButtonControlTypeId && control::kSlider == UIA_SliderControlTypeId &&
              control::kTab == UIA_TabControlTypeId && control::kTabItem == UIA_TabItemControlTypeId &&
              control::kText == UIA_TextControlTypeId && control::kToolBar == UIA_ToolBarControlTypeId &&
              control::kGroup == UIA_GroupControlTypeId && control::kDataGrid == UIA_DataGridControlTypeId &&
              control::kDataItem == UIA_DataItemControlTypeId && control::kPane == UIA_PaneControlTypeId &&
              control::kWindow == UIA_WindowControlTypeId && control::kHyperlink == UIA_HyperlinkControlTypeId &&
              control::kDocument == UIA_DocumentControlTypeId,
              "uia_tree.h's control types are UIA's");

// Text attribute ids and values (UIAutomationClient.h; MinGW's headers lack them).
constexpr TEXTATTRIBUTEID kAttrCulture = 40004, kAttrIsHidden = 40013, kAttrIsReadOnly = 40015;
constexpr TEXTATTRIBUTEID kAttrTextFlowDirections = 40028, kAttrCaretBidiMode = 40039;
constexpr int kFlowRightToLeft = 2;  // FlowDirections_RightToLeft
constexpr double kNoScroll = -1;     // UIA_ScrollPatternNoScroll
constexpr int kHeadingLevel2 = 80052;  // Android gives no level: a heading is a section's
constexpr int kTypeApplication = 2;    // WindowManager.LayoutParams.TYPE_APPLICATION: a dialog's window

// IRawElementProviderAdviseEvents (UIAutomationCore.idl), which MinGW's headers lack.
const IID kIidAdviseEvents = {0xa407b27b, 0x0f6d, 0x4427, {0x92, 0x92, 0x47, 0x3c, 0x7b, 0xf9, 0x32, 0x58}};
struct AdviseEvents : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE AdviseEventAdded(EVENTID event, SAFEARRAY* properties) = 0;
    virtual HRESULT STDMETHODCALLTYPE AdviseEventRemoved(EVENTID event, SAFEARRAY* properties) = 0;
};

// ---- uiautomationcore.dll, loaded when a client first asks --------------------------------------

struct Api {
    bool ok = false;
    LRESULT(WINAPI* return_raw)(HWND, WPARAM, LPARAM, IRawElementProviderSimple*) = nullptr;
    HRESULT(WINAPI* host_from_hwnd)(HWND, IRawElementProviderSimple**) = nullptr;
    HRESULT(WINAPI* raise_event)(IRawElementProviderSimple*, EVENTID) = nullptr;
    HRESULT(WINAPI* raise_property)(IRawElementProviderSimple*, PROPERTYID, VARIANT, VARIANT) = nullptr;
    HRESULT(WINAPI* raise_structure)(IRawElementProviderSimple*, StructureChangeType, int*, int) = nullptr;
    HRESULT(WINAPI* raise_notification)(IRawElementProviderSimple*, NotificationKind, NotificationProcessing, BSTR,
                                        BSTR) = nullptr;  // Windows 10 1709 and later
    BOOL(WINAPI* listening)() = nullptr;
    HRESULT(WINAPI* not_supported)(IUnknown**) = nullptr;
    HRESULT(WINAPI* disconnect)(IRawElementProviderSimple*) = nullptr;  // Windows 8 and later
};

Api g_api;
std::once_flag g_api_once;

template <typename F>
void bind(HMODULE m, const char* name, F* out) {
    *out = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

const Api& api() {
    std::call_once(g_api_once, [] {
        HMODULE m = LoadLibraryW(L"uiautomationcore.dll");
        if (!m) return;
        bind(m, "UiaReturnRawElementProvider", &g_api.return_raw);
        bind(m, "UiaHostProviderFromHwnd", &g_api.host_from_hwnd);
        bind(m, "UiaRaiseAutomationEvent", &g_api.raise_event);
        bind(m, "UiaRaiseAutomationPropertyChangedEvent", &g_api.raise_property);
        bind(m, "UiaRaiseStructureChangedEvent", &g_api.raise_structure);
        bind(m, "UiaRaiseNotificationEvent", &g_api.raise_notification);
        bind(m, "UiaClientsAreListening", &g_api.listening);
        bind(m, "UiaGetReservedNotSupportedValue", &g_api.not_supported);
        bind(m, "UiaDisconnectProvider", &g_api.disconnect);
        g_api.ok = g_api.return_raw && g_api.host_from_hwnd;
    });
    return g_api;
}

bool clients_listening() {
    const Api& a = api();
    return a.ok && a.listening && a.listening();
}

// ---- Small COM helpers --------------------------------------------------------------------------

BSTR bstr(const std::wstring& s) { return SysAllocStringLen(s.data(), static_cast<UINT>(s.size())); }

void set_bstr(VARIANT* v, const std::wstring& s) {
    v->vt = VT_BSTR;
    v->bstrVal = bstr(s);
}

void set_bool(VARIANT* v, bool b) {
    v->vt = VT_BOOL;
    v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
}

void set_i4(VARIANT* v, int i) {
    v->vt = VT_I4;
    v->lVal = i;
}

void set_not_supported(VARIANT* v) {
    IUnknown* u = nullptr;
    if (api().not_supported && SUCCEEDED(api().not_supported(&u)) && u) {
        v->vt = VT_UNKNOWN;
        v->punkVal = u;  // the reserved value is not counted, but UIA expects a reference it may release
        u->AddRef();
    }
}

SAFEARRAY* int_array(const std::vector<int>& v) {
    SAFEARRAY* sa = SafeArrayCreateVector(VT_I4, 0, static_cast<ULONG>(v.size()));
    if (!sa) return nullptr;
    for (LONG i = 0; i < static_cast<LONG>(v.size()); ++i) {
        int x = v[static_cast<size_t>(i)];
        SafeArrayPutElement(sa, &i, &x);
    }
    return sa;
}

SAFEARRAY* unknown_array(const std::vector<IUnknown*>& v) {
    SAFEARRAY* sa = SafeArrayCreateVector(VT_UNKNOWN, 0, static_cast<ULONG>(v.size()));
    if (!sa) return nullptr;
    for (LONG i = 0; i < static_cast<LONG>(v.size()); ++i) SafeArrayPutElement(sa, &i, v[static_cast<size_t>(i)]);
    return sa;  // SafeArrayPutElement took its own references
}

std::vector<int> runtime_id(int64_t id) {
    return {UiaAppendRuntimeId, static_cast<int>(static_cast<uint64_t>(id) >> 32),
            static_cast<int>(static_cast<uint64_t>(id) & 0xffffffffu)};
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

class Element;
class Root;
class TextRange;

// ---- The one window's state -----------------------------------------------------------------------

constexpr int64_t kIdleMs = 60000;          // no client call for a minute: off
constexpr int64_t kFrameRefreshMs = 500;    // a frame with no snapshot since: ask for one
constexpr int64_t kReadingMs = 2000;        // a client called this recently: it is reading
constexpr auto kFirstSnapshotWait = std::chrono::milliseconds(1000);

struct Host {
    std::mutex m;
    HWND hwnd = nullptr;
    Geometry geometry;
    std::shared_ptr<const Tree> tree;  // the newest snapshot
    LCID lcid = 0;
    std::wstring lcid_of;
    Root* root = nullptr;                          // a reference while attached
    std::unordered_map<int64_t, Element*> elements;  // a reference each, while their node exists
    AppSide* app = nullptr;
    bool active = false;
    std::condition_variable tree_cv;

    // What clients subscribed to (AdviseEventAdded), for the property changes worth computing.
    std::map<PROPERTYID, int> advised_properties;
    bool property_advise_seen = false;

    std::atomic<int64_t> last_call{0};
    std::atomic<uint64_t> frames{0};
    uint64_t frames_at_snapshot = 0;
    int64_t last_snapshot = 0, last_refresh_ask = 0;

    // The worker.
    std::mutex qm;
    std::condition_variable qcv;
    std::deque<std::function<void()>> jobs;
    std::wstring pending;  // the newest snapshot's text not yet parsed
    bool pending_queued = false;
    bool started = false;
};

Host& host() {
    static Host* h = new Host;  // for the process's life: its worker never joins at exit
    return *h;
}

void worker_loop();

void queue(std::function<void()> job) {
    Host& h = host();
    std::lock_guard<std::mutex> hold(h.qm);
    if (!h.started) {
        h.started = true;
        std::thread(worker_loop).detach();
    }
    h.jobs.push_back(std::move(job));
    h.qcv.notify_one();
}

std::shared_ptr<const Tree> snapshot(bool wait = false) {
    Host& h = host();
    std::unique_lock<std::mutex> hold(h.m);
    if (!h.tree && wait && h.active) h.tree_cv.wait_for(hold, kFirstSnapshotWait, [&] { return h.tree != nullptr; });
    return h.tree;
}

void ask_app(std::function<void(AppSide*)> call) {
    queue([call] {
        AppSide* app;
        {
            std::lock_guard<std::mutex> hold(host().m);
            app = host().app;
        }
        if (app) call(app);
    });
}

// Every provider call: a client is reading. The first one turns the app's accessibility on.
void touch() {
    Host& h = host();
    const int64_t now = now_ms();
    h.last_call = now;
    bool start = false, refresh = false;
    {
        std::lock_guard<std::mutex> hold(h.m);
        if (!h.hwnd) return;
        if (!h.active) {
            h.active = true;
            start = true;
        } else if (h.frames != h.frames_at_snapshot && now - h.last_snapshot > kFrameRefreshMs &&
                   now - h.last_refresh_ask > kFrameRefreshMs) {
            h.last_refresh_ask = now;
            refresh = true;
        }
    }
    if (start) ask_app([](AppSide* a) { a->activate(true); });
    if (refresh) ask_app([](AppSide* a) { a->refresh(); });
}

void perform(int64_t id, int action, std::wstring text = std::wstring(), int a1 = 0, int a2 = 0, double v = 0) {
    ask_app([=](AppSide* a) { a->perform(id, action, text, a1, a2, v); });
}

bool foreground() {
    HWND hwnd = host().hwnd;
    return hwnd && GetForegroundWindow() == GetAncestor(hwnd, GA_ROOT);
}

bool to_screen(const Box& b, UiaRect* out) {
    *out = UiaRect{0, 0, 0, 0};
    RECT r{};
    if (b.empty() || !host().geometry.to_screen || !host().geometry.to_screen(b, &r)) return false;
    out->left = r.left;
    out->top = r.top;
    out->width = r.right - r.left;
    out->height = r.bottom - r.top;
    return true;
}

IRawElementProviderSimple* element_ref(int64_t id);  // a new reference, or null
IRawElementProviderSimple* root_ref();

// ---- An element: one node --------------------------------------------------------------------------

class Element final : public IRawElementProviderSimple,
                      public IRawElementProviderFragment,
                      public IInvokeProvider,
                      public IToggleProvider,
                      public IValueProvider,
                      public IRangeValueProvider,
                      public IExpandCollapseProvider,
                      public IScrollProvider,
                      public IScrollItemProvider,
                      public ISelectionItemProvider,
                      public ISelectionProvider,
                      public ITextProvider2 {
public:
    explicit Element(int64_t id) : id_(id) {}
    int64_t id() const { return id_; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (riid == IID_IUnknown || riid == IID_IRawElementProviderSimple) {
            *out = static_cast<IRawElementProviderSimple*>(this);
        } else if (riid == IID_IRawElementProviderFragment) {
            *out = static_cast<IRawElementProviderFragment*>(this);
        } else {
            *out = pattern_interface(riid);
        }
        if (!*out) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    // IRawElementProviderSimple
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* out) override {
        if (!out) return E_POINTER;
        *out = ProviderOptions_ServerSideProvider;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** out) override;
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* out) override;
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return S_OK;
    }

    // IRawElementProviderFragment
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** out) override;
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** out) override {
        if (!out) return E_POINTER;
        *out = int_array(runtime_id(id_));
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* out) override {
        if (!out) return E_POINTER;
        touch();
        auto t = snapshot();
        const Node* n = t ? t->find(id_) : nullptr;
        if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
        to_screen(n->bounds, out);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override {
        return act([&](const Node& n) {
            if (!n.can_do(kCanFocus)) return n.has(kFocused) ? S_OK : static_cast<HRESULT>(UIA_E_INVALIDOPERATION);
            perform(id_, action::kFocus);
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** out) override;

    // IInvokeProvider
    HRESULT STDMETHODCALLTYPE Invoke() override { return click(); }

    // IToggleProvider
    HRESULT STDMETHODCALLTYPE Toggle() override { return click(); }
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* out) override {
        return read([&](const Node& n) { *out = n.has(kChecked) ? ToggleState_On : ToggleState_Off; }, out);
    }

    // IValueProvider
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override {
        std::wstring v = value ? value : L"";
        return act([&](const Node& n) {
            if (!n.has(kEditable) || !n.can_do(kCanSetText)) return static_cast<HRESULT>(UIA_E_INVALIDOPERATION);
            perform(id_, action::kSetText, v);
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* out) override {
        return read(
            [&](const Node& n) {
                std::wstring v = n.has(kShowingHint) ? std::wstring() : n.text;
                if (!n.has(kEditable) && v.empty()) v = n.name;  // a spinner: what it shows
                *out = bstr(v);
            },
            out);
    }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(WINBOOL* out) override {
        return read(
            [&](const Node& n) {
                *out = (n.patterns & kPatRangeValue) && !(n.patterns & kPatValue)
                           ? !n.can_do(kCanSetProgress)
                           : !(n.has(kEditable) && n.can_do(kCanSetText) && n.has(kEnabled));
            },
            out);
    }

    // IRangeValueProvider (get_IsReadOnly is shared with IValueProvider's: one node has one of them)
    HRESULT STDMETHODCALLTYPE SetValue(double value) override {
        return act([&](const Node& n) {
            if (!n.range.present || !n.can_do(kCanSetProgress)) return static_cast<HRESULT>(UIA_E_INVALIDOPERATION);
            if (value < n.range.min || value > n.range.max) return E_INVALIDARG;
            perform(id_, action::kSetProgress, std::wstring(), 0, 0, value);
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE get_Value(double* out) override {
        return read([&](const Node& n) { *out = n.range.now; }, out);
    }
    HRESULT STDMETHODCALLTYPE get_Maximum(double* out) override {
        return read([&](const Node& n) { *out = n.range.max; }, out);
    }
    HRESULT STDMETHODCALLTYPE get_Minimum(double* out) override {
        return read([&](const Node& n) { *out = n.range.min; }, out);
    }
    // A step as AbsSeekBar's keys take one: a twentieth of the range, at least 1 for whole numbers.
    HRESULT STDMETHODCALLTYPE get_SmallChange(double* out) override {
        return read([&](const Node& n) { *out = step(n); }, out);
    }
    HRESULT STDMETHODCALLTYPE get_LargeChange(double* out) override {
        return read([&](const Node& n) { *out = std::max(step(n), (n.range.max - n.range.min) / 10); }, out);
    }

    // IExpandCollapseProvider
    HRESULT STDMETHODCALLTYPE Expand() override { return simple(kCanExpand, action::kExpand); }
    HRESULT STDMETHODCALLTYPE Collapse() override { return simple(kCanCollapse, action::kCollapse); }
    HRESULT STDMETHODCALLTYPE get_ExpandCollapseState(ExpandCollapseState* out) override {
        return read(
            [&](const Node& n) {
                const bool e = n.can_do(kCanExpand), c = n.can_do(kCanCollapse);
                *out = e && c ? ExpandCollapseState_PartiallyExpanded
                       : c    ? ExpandCollapseState_Expanded
                       : e    ? ExpandCollapseState_Collapsed
                              : ExpandCollapseState_LeafNode;
            },
            out);
    }

    // IScrollProvider
    HRESULT STDMETHODCALLTYPE Scroll(ScrollAmount horizontal, ScrollAmount vertical) override;
    HRESULT STDMETHODCALLTYPE SetScrollPercent(double h, double v) override {
        // Android tells a screen reader no scroll position, only that a view can scroll.
        return h == kNoScroll && v == kNoScroll ? S_OK : UIA_E_INVALIDOPERATION;
    }
    HRESULT STDMETHODCALLTYPE get_HorizontalScrollPercent(double* out) override { return no_scroll(out); }
    HRESULT STDMETHODCALLTYPE get_VerticalScrollPercent(double* out) override { return no_scroll(out); }
    HRESULT STDMETHODCALLTYPE get_HorizontalViewSize(double* out) override { return full_view(out); }
    HRESULT STDMETHODCALLTYPE get_VerticalViewSize(double* out) override { return full_view(out); }
    HRESULT STDMETHODCALLTYPE get_HorizontallyScrollable(WINBOOL* out) override {
        return read([&](const Node& n) { *out = horizontal(n); }, out);
    }
    HRESULT STDMETHODCALLTYPE get_VerticallyScrollable(WINBOOL* out) override {
        return read([&](const Node& n) { *out = vertical(n); }, out);
    }

    // IScrollItemProvider
    HRESULT STDMETHODCALLTYPE ScrollIntoView() override { return simple(kCanShowOnScreen, action::kShowOnScreen); }

    // ISelectionItemProvider
    HRESULT STDMETHODCALLTYPE Select() override {
        return act([&](const Node& n) {
            if (n.can_do(kCanSelect)) {
                perform(id_, action::kSelect);
            } else if (n.can_do(kCanClick)) {
                perform(id_, action::kClick);  // a radio button, a tab: selected by a tap
            } else {
                return static_cast<HRESULT>(UIA_E_INVALIDOPERATION);
            }
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE AddToSelection() override { return Select(); }
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override {
        return simple(kCanClearSelection, action::kClearSelection);
    }
    HRESULT STDMETHODCALLTYPE get_IsSelected(WINBOOL* out) override {
        return read([&](const Node& n) { *out = selected(n); }, out);
    }
    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** out) override {
        return read([&](const Node& n) { *out = n.parent ? element_ref(n.parent) : nullptr; }, out);
    }

    // ISelectionProvider (GetSelection is shared with ITextProvider's: told apart by the node)
    HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** out) override;
    HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(WINBOOL* out) override {
        return read([&](const Node& n) { *out = n.collection.present && n.collection.selection_mode == 2; }, out);
    }
    HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(WINBOOL* out) override {
        return read([&](const Node&) { *out = FALSE; }, out);
    }

    // ITextProvider, ITextProvider2
    HRESULT STDMETHODCALLTYPE GetVisibleRanges(SAFEARRAY** out) override;
    HRESULT STDMETHODCALLTYPE RangeFromChild(IRawElementProviderSimple*, ITextRangeProvider** out) override {
        if (out) *out = nullptr;
        return UIA_E_INVALIDOPERATION;  // an Android text has no child elements in it
    }
    HRESULT STDMETHODCALLTYPE RangeFromPoint(UiaPoint point, ITextRangeProvider** out) override;
    HRESULT STDMETHODCALLTYPE get_DocumentRange(ITextRangeProvider** out) override;
    HRESULT STDMETHODCALLTYPE get_SupportedTextSelection(SupportedTextSelection* out) override {
        return read([&](const Node&) { *out = SupportedTextSelection_Single; }, out);
    }
    HRESULT STDMETHODCALLTYPE RangeFromAnnotation(IRawElementProviderSimple*, ITextRangeProvider** out) override {
        if (out) *out = nullptr;
        return UIA_E_INVALIDOPERATION;
    }
    HRESULT STDMETHODCALLTYPE GetCaretRange(WINBOOL* active, ITextRangeProvider** out) override;

private:
    ~Element() = default;

    void* pattern_interface(REFIID riid) {
        auto t = snapshot();
        const Node* n = t ? t->find(id_) : nullptr;
        const uint32_t p = n ? n->patterns : 0;
        if (riid == IID_IInvokeProvider && (p & kPatInvoke)) return static_cast<IInvokeProvider*>(this);
        if (riid == IID_IToggleProvider && (p & kPatToggle)) return static_cast<IToggleProvider*>(this);
        if (riid == IID_IValueProvider && (p & kPatValue)) return static_cast<IValueProvider*>(this);
        if (riid == IID_IRangeValueProvider && (p & kPatRangeValue)) return static_cast<IRangeValueProvider*>(this);
        if (riid == IID_IExpandCollapseProvider && (p & kPatExpandCollapse)) {
            return static_cast<IExpandCollapseProvider*>(this);
        }
        if (riid == IID_IScrollProvider && (p & kPatScroll)) return static_cast<IScrollProvider*>(this);
        if (riid == IID_IScrollItemProvider && (p & kPatScrollItem)) return static_cast<IScrollItemProvider*>(this);
        if (riid == IID_ISelectionItemProvider && (p & kPatSelectionItem)) {
            return static_cast<ISelectionItemProvider*>(this);
        }
        if (riid == IID_ISelectionProvider && (p & kPatSelection)) return static_cast<ISelectionProvider*>(this);
        if (riid == IID_ITextProvider && (p & kPatText)) return static_cast<ITextProvider*>(static_cast<ITextProvider2*>(this));
        if (riid == IID_ITextProvider2 && (p & kPatText)) return static_cast<ITextProvider2*>(this);
        return nullptr;
    }

    // Reads the node; UIA_E_ELEMENTNOTAVAILABLE once it is gone.
    template <typename F, typename T>
    HRESULT read(F f, T* out) {
        if (!out) return E_POINTER;
        touch();
        auto t = snapshot();
        const Node* n = t ? t->find(id_) : nullptr;
        if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
        f(*n);
        return S_OK;
    }

    template <typename F>
    HRESULT act(F f) {
        touch();
        auto t = snapshot();
        const Node* n = t ? t->find(id_) : nullptr;
        if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
        if (!n->has(kEnabled)) return UIA_E_ELEMENTNOTENABLED;
        return f(*n);
    }

    HRESULT simple(uint32_t can, int android_action) {
        return act([&](const Node& n) {
            if (!n.can_do(can)) return static_cast<HRESULT>(UIA_E_INVALIDOPERATION);
            perform(id_, android_action);
            return S_OK;
        });
    }

    HRESULT click() { return simple(kCanClick, action::kClick); }

    static double step(const Node& n) {
        const double span = n.range.max - n.range.min;
        double s = span / 20;
        if (n.range.type == 0) s = std::max(1.0, std::round(s));  // RANGE_TYPE_INT
        return s;
    }

    static bool selected(const Node& n) {
        if (n.control_type == control::kRadioButton) return n.has(kChecked);
        return n.has(kSelected) || n.item.selected;
    }

    static bool horizontal_kind(const Node& n) {
        const std::wstring& c = n.class_name;
        auto ends = [&](const wchar_t* s) {
            const size_t l = wcslen(s);
            return c.size() >= l && c.compare(c.size() - l, l, s) == 0;
        };
        return ends(L"HorizontalScrollView") || ends(L"ViewPager") || ends(L"ViewPager2") ||
               (n.collection.present && n.collection.rows == 1 && n.collection.columns > 1);
    }

    static bool horizontal(const Node& n) {
        return n.can_do(kCanScrollLeft | kCanScrollRight) ||
               (horizontal_kind(n) && n.can_do(kCanScrollForward | kCanScrollBackward));
    }

    static bool vertical(const Node& n) {
        return n.can_do(kCanScrollUp | kCanScrollDown | kCanPageUp | kCanPageDown) ||
               (!horizontal_kind(n) && n.can_do(kCanScrollForward | kCanScrollBackward));
    }

    static HRESULT no_scroll(double* out) {
        if (!out) return E_POINTER;
        *out = kNoScroll;
        return S_OK;
    }
    static HRESULT full_view(double* out) {
        if (!out) return E_POINTER;
        *out = 100;
        return S_OK;
    }

    HRESULT text_range(int start, int end, ITextRangeProvider** out);

    const int64_t id_;
    LONG refs_ = 1;
};

// ---- A range of an element's text ---------------------------------------------------------------------

class TextRange final : public ITextRangeProvider {
public:
    TextRange(int64_t id, int start, int end) : id_(id), start_(start), end_(end) {}
    int64_t id() const { return id_; }
    int start() const { return start_; }
    int end() const { return end_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (riid == IID_IUnknown || riid == IID_ITextRangeProvider) {
            *out = static_cast<ITextRangeProvider*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    HRESULT STDMETHODCALLTYPE Clone(ITextRangeProvider** out) override {
        if (!out) return E_POINTER;
        *out = new TextRange(id_, start_, end_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Compare(ITextRangeProvider* other, WINBOOL* out) override {
        if (!out) return E_POINTER;
        TextRange* o = from(other);
        if (!o) return E_INVALIDARG;
        *out = o->id_ == id_ && o->start_ == start_ && o->end_ == end_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CompareEndpoints(TextPatternRangeEndpoint endpoint, ITextRangeProvider* target,
                                               TextPatternRangeEndpoint target_endpoint, int* out) override {
        if (!out) return E_POINTER;
        TextRange* o = from(target);
        if (!o || o->id_ != id_) return E_INVALIDARG;
        const int a = endpoint == TextPatternRangeEndpoint_Start ? start_ : end_;
        const int b = target_endpoint == TextPatternRangeEndpoint_Start ? o->start_ : o->end_;
        *out = a - b;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ExpandToEnclosingUnit(TextUnit unit) override {
        return with_model([&](const Node&, const TextModel& m) {
            m.expand(static_cast<Unit>(unit), &start_, &end_);
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE FindAttribute(TEXTATTRIBUTEID attribute, VARIANT value, WINBOOL,
                                            ITextRangeProvider** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        VARIANT mine;
        VariantInit(&mine);
        HRESULT hr = GetAttributeValue(attribute, &mine);
        if (FAILED(hr)) return hr;
        // One run of attributes covers the whole text: the range matches whole or not at all.
        if (VarCmp(&mine, &value, LOCALE_USER_DEFAULT, 0) == static_cast<HRESULT>(VARCMP_EQ)) {
            *out = new TextRange(id_, start_, end_);
        }
        VariantClear(&mine);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindText(BSTR text, WINBOOL backward, WINBOOL ignore_case,
                                       ITextRangeProvider** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        std::wstring needle = text ? std::wstring(text, SysStringLen(text)) : std::wstring();
        return with_model([&](const Node&, const TextModel& m) {
            const int at = m.find(needle, start_, end_, backward != FALSE, ignore_case != FALSE);
            if (at >= 0) *out = new TextRange(id_, at, at + static_cast<int>(needle.size()));
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE GetAttributeValue(TEXTATTRIBUTEID attribute, VARIANT* out) override {
        if (!out) return E_POINTER;
        VariantInit(out);
        return with_model([&](const Node& n, const TextModel&) {
            switch (attribute) {
                case kAttrCulture: {
                    std::lock_guard<std::mutex> hold(host().m);
                    set_i4(out, static_cast<int>(host().lcid));
                    break;
                }
                case kAttrIsReadOnly: set_bool(out, !n.has(kEditable)); break;
                case kAttrIsHidden: set_bool(out, false); break;
                case kAttrTextFlowDirections: set_i4(out, n.has(kRtl) ? kFlowRightToLeft : 0); break;
                case kAttrCaretBidiMode: set_i4(out, n.has(kRtl) ? 1 : 0); break;  // CaretBidiMode_RTL
                default: set_not_supported(out); break;
            }
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE GetBoundingRectangles(SAFEARRAY** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return with_model([&](const Node&, const TextModel& m) {
            std::vector<UiaRect> rects;
            for (const Box& b : m.boxes(start_, end_)) {
                UiaRect r;
                if (to_screen(b, &r)) rects.push_back(r);
            }
            *out = SafeArrayCreateVector(VT_R8, 0, static_cast<ULONG>(rects.size() * 4));
            if (!*out) return static_cast<HRESULT>(E_OUTOFMEMORY);
            LONG i = 0;
            for (const UiaRect& r : rects) {
                for (double d : {r.left, r.top, r.width, r.height}) {
                    SafeArrayPutElement(*out, &i, &d);
                    ++i;
                }
            }
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE GetEnclosingElement(IRawElementProviderSimple** out) override {
        if (!out) return E_POINTER;
        *out = element_ref(id_);
        return *out ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
    }
    HRESULT STDMETHODCALLTYPE GetText(int max_length, BSTR* out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return with_model([&](const Node&, const TextModel& m) {
            std::wstring s = m.text().substr(static_cast<size_t>(start_), static_cast<size_t>(end_ - start_));
            if (max_length >= 0 && static_cast<int>(s.size()) > max_length) s.resize(static_cast<size_t>(max_length));
            *out = bstr(s);
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE Move(TextUnit unit, int count, int* out) override {
        if (!out) return E_POINTER;
        return with_model([&](const Node&, const TextModel& m) {
            *out = m.move(static_cast<Unit>(unit), count, &start_, &end_);
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE MoveEndpointByUnit(TextPatternRangeEndpoint endpoint, TextUnit unit, int count,
                                                 int* out) override {
        if (!out) return E_POINTER;
        return with_model([&](const Node&, const TextModel& m) {
            if (endpoint == TextPatternRangeEndpoint_Start) {
                *out = m.move_endpoint(static_cast<Unit>(unit), count, &start_);
                if (start_ > end_) end_ = start_;
            } else {
                *out = m.move_endpoint(static_cast<Unit>(unit), count, &end_);
                if (end_ < start_) start_ = end_;
            }
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE MoveEndpointByRange(TextPatternRangeEndpoint endpoint, ITextRangeProvider* target,
                                                  TextPatternRangeEndpoint target_endpoint) override {
        TextRange* o = from(target);
        if (!o || o->id_ != id_) return E_INVALIDARG;
        const int at = target_endpoint == TextPatternRangeEndpoint_Start ? o->start_ : o->end_;
        if (endpoint == TextPatternRangeEndpoint_Start) {
            start_ = at;
            if (end_ < start_) end_ = start_;
        } else {
            end_ = at;
            if (start_ > end_) start_ = end_;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Select() override {
        return with_model([&](const Node& n, const TextModel&) {
            if (!n.can_do(kCanSetSelection)) return static_cast<HRESULT>(UIA_E_INVALIDOPERATION);
            perform(id_, action::kSetSelection, std::wstring(), start_, end_);
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE AddToSelection() override { return UIA_E_INVALIDOPERATION; }  // one selection only
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override { return UIA_E_INVALIDOPERATION; }
    HRESULT STDMETHODCALLTYPE ScrollIntoView(WINBOOL) override {
        return with_model([&](const Node& n, const TextModel&) {
            if (n.can_do(kCanShowOnScreen)) perform(id_, action::kShowOnScreen);
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE GetChildren(SAFEARRAY** out) override {
        if (!out) return E_POINTER;
        *out = unknown_array({});
        return S_OK;
    }

    static TextRange* from(ITextRangeProvider* p);

private:
    ~TextRange() = default;

    // The node's text now, with this range clamped into it (the text may have changed under it).
    template <typename F>
    HRESULT with_model(F f) {
        touch();
        auto t = snapshot();
        const Node* n = t ? t->find(id_) : nullptr;
        if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
        TextModel m(*n);
        start_ = std::clamp(start_, 0, m.length());
        end_ = std::clamp(end_, start_, m.length());
        return f(*n, m);
    }

    const int64_t id_;
    int start_, end_;
    LONG refs_ = 1;
};

// Ranges are only ever ours: one class, and UIA hands back what it was given.
TextRange* TextRange::from(ITextRangeProvider* p) { return p ? static_cast<TextRange*>(p) : nullptr; }

// ---- The fragment root: the window ---------------------------------------------------------------------

class Root final : public IRawElementProviderSimple,
                   public IRawElementProviderFragment,
                   public IRawElementProviderFragmentRoot,
                   public AdviseEvents {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (riid == IID_IUnknown || riid == IID_IRawElementProviderSimple) {
            *out = static_cast<IRawElementProviderSimple*>(this);
        } else if (riid == IID_IRawElementProviderFragment) {
            *out = static_cast<IRawElementProviderFragment*>(this);
        } else if (riid == IID_IRawElementProviderFragmentRoot) {
            *out = static_cast<IRawElementProviderFragmentRoot*>(this);
        } else if (riid == kIidAdviseEvents) {
            *out = static_cast<AdviseEvents*>(this);
        }
        if (!*out) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* out) override {
        if (!out) return E_POINTER;
        *out = ProviderOptions_ServerSideProvider;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return S_OK;
    }
    // The window's own provider (its name, its frame) answers for it.
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID, VARIANT* out) override {
        if (!out) return E_POINTER;
        VariantInit(out);
        touch();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        HWND hwnd = host().hwnd;
        if (!hwnd || !api().ok) return S_OK;
        return api().host_from_hwnd(hwnd, out);
    }

    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        touch();
        if (direction != NavigateDirection_FirstChild && direction != NavigateDirection_LastChild) return S_OK;
        auto t = snapshot(true);
        if (!t || t->windows.empty()) return S_OK;
        const int64_t id = direction == NavigateDirection_FirstChild ? t->windows.front().root : t->windows.back().root;
        return fragment(id, out);
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;  // the window's
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* out) override {
        if (!out) return E_POINTER;
        *out = UiaRect{0, 0, 0, 0};  // the window's
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** out) override {
        if (!out) return E_POINTER;
        AddRef();
        *out = this;
        return S_OK;
    }

    // The deepest visible node under the point, the topmost window's first; in a window, a later
    // child is drawn over an earlier one.
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        touch();
        auto t = snapshot(true);
        POINT p{};
        if (!t || !host().geometry.to_frame ||
            !host().geometry.to_frame(POINT{static_cast<LONG>(std::floor(x)), static_cast<LONG>(std::floor(y))}, &p)) {
            return S_OK;
        }
        std::function<int64_t(int64_t)> hit = [&](int64_t id) -> int64_t {
            const Node* n = t->find(id);
            if (!n || !n->has(kVisible) || !n->bounds.contains(p.x, p.y)) return 0;
            for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) {
                if (int64_t deeper = hit(*it)) return deeper;
            }
            return id;
        };
        for (auto w = t->windows.rbegin(); w != t->windows.rend(); ++w) {
            if (int64_t id = hit(w->root)) return fragment(id, out);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        touch();
        auto t = snapshot(true);
        if (!t || !t->focused) return S_OK;
        return fragment(t->focused, out);
    }

    HRESULT STDMETHODCALLTYPE AdviseEventAdded(EVENTID event, SAFEARRAY* properties) override {
        advise(event, properties, 1);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE AdviseEventRemoved(EVENTID event, SAFEARRAY* properties) override {
        advise(event, properties, -1);
        return S_OK;
    }

    static HRESULT fragment(int64_t id, IRawElementProviderFragment** out) {
        IRawElementProviderSimple* e = element_ref(id);
        if (!e) return S_OK;
        HRESULT hr = e->QueryInterface(IID_IRawElementProviderFragment, reinterpret_cast<void**>(out));
        e->Release();
        return hr;
    }

private:
    ~Root() = default;

    static void advise(EVENTID event, SAFEARRAY* properties, int delta) {
        if (event != UIA_AutomationPropertyChangedEventId || !properties) return;
        LONG lo = 0, hi = -1;
        SafeArrayGetLBound(properties, 1, &lo);
        SafeArrayGetUBound(properties, 1, &hi);
        Host& h = host();
        std::lock_guard<std::mutex> hold(h.m);
        h.property_advise_seen = true;
        for (LONG i = lo; i <= hi; ++i) {
            PROPERTYID p = 0;
            if (SUCCEEDED(SafeArrayGetElement(properties, &i, &p))) h.advised_properties[p] += delta;
        }
    }

    LONG refs_ = 1;
};

// ---- Element methods that need the root or other elements ---------------------------------------------------

IRawElementProviderSimple* element_ref(int64_t id) {
    Host& h = host();
    std::lock_guard<std::mutex> hold(h.m);
    if (!h.hwnd || !h.tree || !h.tree->find(id)) return nullptr;
    Element*& e = h.elements[id];
    if (!e) e = new Element(id);  // the map's reference
    e->AddRef();
    return static_cast<IRawElementProviderSimple*>(e);
}

IRawElementProviderSimple* root_ref() {
    Host& h = host();
    std::lock_guard<std::mutex> hold(h.m);
    if (!h.root) return nullptr;
    h.root->AddRef();
    return static_cast<IRawElementProviderSimple*>(h.root);
}

HRESULT Element::get_FragmentRoot(IRawElementProviderFragmentRoot** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    IRawElementProviderSimple* r = root_ref();
    if (!r) return UIA_E_ELEMENTNOTAVAILABLE;
    HRESULT hr = r->QueryInterface(IID_IRawElementProviderFragmentRoot, reinterpret_cast<void**>(out));
    r->Release();
    return hr;
}

HRESULT Element::Navigate(NavigateDirection direction, IRawElementProviderFragment** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    touch();
    auto t = snapshot();
    const Node* n = t ? t->find(id_) : nullptr;
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    // A window's root: its parent is the fragment root, its siblings the other windows.
    std::vector<int64_t> siblings;
    const Node* parent = n->parent ? t->find(n->parent) : nullptr;
    if (parent) {
        siblings = parent->children;
    } else {
        for (const Window& w : t->windows) siblings.push_back(w.root);
    }
    int64_t target = 0;
    switch (direction) {
        case NavigateDirection_Parent:
            if (!parent) {
                IRawElementProviderSimple* r = root_ref();
                if (!r) return S_OK;
                HRESULT hr = r->QueryInterface(IID_IRawElementProviderFragment, reinterpret_cast<void**>(out));
                r->Release();
                return hr;
            }
            target = parent->id;
            break;
        case NavigateDirection_FirstChild:
            if (!n->children.empty()) target = n->children.front();
            break;
        case NavigateDirection_LastChild:
            if (!n->children.empty()) target = n->children.back();
            break;
        case NavigateDirection_NextSibling:
        case NavigateDirection_PreviousSibling: {
            auto it = std::find(siblings.begin(), siblings.end(), id_);
            if (it == siblings.end()) break;
            if (direction == NavigateDirection_NextSibling) {
                if (it + 1 != siblings.end()) target = *(it + 1);
            } else if (it != siblings.begin()) {
                target = *(it - 1);
            }
            break;
        }
    }
    return target ? Root::fragment(target, out) : S_OK;
}

HRESULT Element::GetPatternProvider(PATTERNID pattern, IUnknown** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    touch();
    static const std::pair<PATTERNID, const IID*> kPatterns[] = {
        {UIA_InvokePatternId, &IID_IInvokeProvider},
        {UIA_TogglePatternId, &IID_IToggleProvider},
        {UIA_ValuePatternId, &IID_IValueProvider},
        {UIA_RangeValuePatternId, &IID_IRangeValueProvider},
        {UIA_ExpandCollapsePatternId, &IID_IExpandCollapseProvider},
        {UIA_ScrollPatternId, &IID_IScrollProvider},
        {UIA_ScrollItemPatternId, &IID_IScrollItemProvider},
        {UIA_SelectionItemPatternId, &IID_ISelectionItemProvider},
        {UIA_SelectionPatternId, &IID_ISelectionProvider},
        {UIA_TextPatternId, &IID_ITextProvider},
        {UIA_TextPattern2Id, &IID_ITextProvider2},
    };
    for (const auto& p : kPatterns) {
        if (p.first != pattern) continue;
        // The pattern's own interface, when the node supports it now.
        if (pattern_interface(*p.second)) return QueryInterface(*p.second, reinterpret_cast<void**>(out));
        return S_OK;
    }
    return S_OK;
}

HRESULT Element::GetPropertyValue(PROPERTYID property, VARIANT* out) {
    if (!out) return E_POINTER;
    VariantInit(out);
    touch();
    auto t = snapshot();
    const Node* n = t ? t->find(id_) : nullptr;
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    const Window* window = t->window_of_root(n->id);
    switch (property) {
        case UIA_ControlTypePropertyId: set_i4(out, n->control_type); break;
        case UIA_NamePropertyId:
            if (!n->name.empty()) set_bstr(out, n->name);
            break;
        case UIA_AutomationIdPropertyId:
            if (!n->resource_id.empty()) set_bstr(out, n->resource_id);
            break;
        case UIA_ClassNamePropertyId:
            if (!n->class_name.empty()) set_bstr(out, n->class_name);
            break;
        case UIA_LocalizedControlTypePropertyId:
            if (!n->role.empty()) set_bstr(out, n->role);  // the app's own word (roleDescription)
            break;
        case UIA_HelpTextPropertyId:
            if (!n->help.empty()) set_bstr(out, n->help);
            break;
        case UIA_ItemStatusPropertyId:
            if (!n->status.empty()) set_bstr(out, n->status);
            break;
        case UIA_FrameworkIdPropertyId: set_bstr(out, L"Android"); break;
        case UIA_IsEnabledPropertyId: set_bool(out, n->has(kEnabled)); break;
        case UIA_IsKeyboardFocusablePropertyId: set_bool(out, n->has(kFocusable)); break;
        case UIA_HasKeyboardFocusPropertyId: set_bool(out, n->has(kFocused) && t->focused == id_ && foreground()); break;
        case UIA_IsOffscreenPropertyId: set_bool(out, !n->has(kVisible) || n->bounds.empty()); break;
        case UIA_IsPasswordPropertyId: set_bool(out, n->has(kPassword)); break;
        case UIA_IsControlElementPropertyId: set_bool(out, n->control_element); break;
        case UIA_IsContentElementPropertyId: set_bool(out, n->content_element); break;
        case UIA_LiveSettingPropertyId:
            if (n->live) set_i4(out, n->live == 2 ? Assertive : Polite);
            break;
        case UIA_HeadingLevelPropertyId:
            if (n->has(kHeading) || n->item.heading) set_i4(out, kHeadingLevel2);
            break;
        case UIA_IsDialogPropertyId:
            if (window) set_bool(out, window->type == kTypeApplication);
            break;
        case UIA_CulturePropertyId: {
            std::lock_guard<std::mutex> hold(host().m);
            if (host().lcid) set_i4(out, static_cast<int>(host().lcid));
            break;
        }
        case UIA_IsDataValidForFormPropertyId:
            if (n->has(kEditable)) set_bool(out, !n->has(kContentInvalid) && n->error.empty());
            break;
        case UIA_PositionInSetPropertyId:
        case UIA_SizeOfSetPropertyId: {
            // A list's item: "3 of 12", by row (by column in a one-row list).
            const Node* parent = n->parent ? t->find(n->parent) : nullptr;
            if (!n->item.present || !parent || !parent->collection.present) break;
            const bool across = parent->collection.rows == 1 && parent->collection.columns > 1;
            const int size = across ? parent->collection.columns : parent->collection.rows;
            const int pos = (across ? n->item.column : n->item.row) + 1;
            if (size > 0 && pos > 0) set_i4(out, property == UIA_PositionInSetPropertyId ? pos : size);
            break;
        }
        case UIA_LabeledByPropertyId:
            if (n->labeled_by) {
                if (IRawElementProviderSimple* l = element_ref(n->labeled_by)) {
                    out->vt = VT_UNKNOWN;
                    out->punkVal = l;
                }
            }
            break;
        default: break;
    }
    return S_OK;
}

HRESULT Element::Scroll(ScrollAmount h, ScrollAmount v) {
    return act([&](const Node& n) {
        auto pick = [&](ScrollAmount amount, bool vert) -> int {
            const bool more = amount == ScrollAmount_LargeIncrement || amount == ScrollAmount_SmallIncrement;
            const bool less = amount == ScrollAmount_LargeDecrement || amount == ScrollAmount_SmallDecrement;
            const bool large = amount == ScrollAmount_LargeIncrement || amount == ScrollAmount_LargeDecrement;
            if (!more && !less) return 0;
            if (vert) {
                if (large && more && n.can_do(kCanPageDown)) return action::kPageDown;
                if (large && less && n.can_do(kCanPageUp)) return action::kPageUp;
                if (more && n.can_do(kCanScrollDown)) return action::kScrollDown;
                if (less && n.can_do(kCanScrollUp)) return action::kScrollUp;
                if (horizontal_kind(n)) return 0;
            } else {
                if (more && n.can_do(kCanScrollRight)) return action::kScrollRight;
                if (less && n.can_do(kCanScrollLeft)) return action::kScrollLeft;
                if (!horizontal_kind(n)) return 0;
            }
            if (more && n.can_do(kCanScrollForward)) return action::kScrollForward;
            if (less && n.can_do(kCanScrollBackward)) return action::kScrollBackward;
            return 0;
        };
        const int a = pick(v, true), b = pick(h, false);
        if ((v != ScrollAmount_NoAmount && !a) || (h != ScrollAmount_NoAmount && !b)) {
            return static_cast<HRESULT>(UIA_E_INVALIDOPERATION);
        }
        if (a) perform(id_, a);
        if (b) perform(id_, b);
        return S_OK;
    });
}

HRESULT Element::text_range(int start, int end, ITextRangeProvider** out) {
    *out = new TextRange(id_, start, end);
    return S_OK;
}

HRESULT Element::GetSelection(SAFEARRAY** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    touch();
    auto t = snapshot();
    const Node* n = t ? t->find(id_) : nullptr;
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    std::vector<IUnknown*> items;
    if (n->patterns & kPatText) {
        // The Text pattern's: the editor's selection, or its caret.
        if (n->sel_start >= 0 || n->sel_end >= 0) {
            TextModel m(*n);
            int a = std::clamp(std::min(n->sel_start, n->sel_end), 0, m.length());
            int b = std::clamp(std::max(n->sel_start, n->sel_end), 0, m.length());
            if (a < 0) a = b;
            items.push_back(new TextRange(id_, a, b));
        }
    } else {
        for (int64_t c : n->children) {
            const Node* k = t->find(c);
            if (k && (k->patterns & kPatSelectionItem) && selected(*k)) {
                if (IRawElementProviderSimple* e = element_ref(c)) items.push_back(e);
            }
        }
    }
    *out = unknown_array(items);
    for (IUnknown* u : items) u->Release();
    return *out ? S_OK : E_OUTOFMEMORY;
}

HRESULT Element::GetVisibleRanges(SAFEARRAY** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    touch();
    auto t = snapshot();
    const Node* n = t ? t->find(id_) : nullptr;
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    TextModel m(*n);
    IUnknown* r = new TextRange(id_, 0, m.length());
    *out = unknown_array({r});
    r->Release();
    return S_OK;
}

HRESULT Element::RangeFromPoint(UiaPoint point, ITextRangeProvider** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    touch();
    auto t = snapshot();
    const Node* n = t ? t->find(id_) : nullptr;
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    TextModel m(*n);
    POINT p{};
    int at = 0;
    if (host().geometry.to_frame &&
        host().geometry.to_frame(POINT{static_cast<LONG>(point.x), static_cast<LONG>(point.y)}, &p)) {
        at = m.offset_at(p.x, p.y);
    }
    return text_range(at, at, out);
}

HRESULT Element::get_DocumentRange(ITextRangeProvider** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    touch();
    auto t = snapshot();
    const Node* n = t ? t->find(id_) : nullptr;
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    return text_range(0, TextModel(*n).length(), out);
}

HRESULT Element::GetCaretRange(WINBOOL* is_active, ITextRangeProvider** out) {
    if (!out || !is_active) return E_POINTER;
    *out = nullptr;
    touch();
    auto t = snapshot();
    const Node* n = t ? t->find(id_) : nullptr;
    if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
    TextModel m(*n);
    *is_active = n->has(kFocused) && t->focused == id_;
    const int caret = n->sel_end >= 0 ? std::clamp(n->sel_end, 0, m.length()) : m.length();
    return text_range(caret, caret, out);
}

// ---- The worker: snapshots parsed, events raised, the app called -------------------------------------------

void raise(const Tree& t, const std::vector<Event>& events) {
    const Api& a = api();
    Host& h = host();
    std::map<PROPERTYID, int> advised;
    bool advise_seen;
    {
        std::lock_guard<std::mutex> hold(h.m);
        advised = h.advised_properties;
        advise_seen = h.property_advise_seen;
    }
    auto wants = [&](PROPERTYID p) {
        auto it = advised.find(p);
        return !advise_seen || (it != advised.end() && it->second > 0);
    };
    for (const Event& e : events) {
        if (e.kind == EventKind::kGone) continue;
        IRawElementProviderSimple* p = e.id ? element_ref(e.id) : root_ref();
        if (!p) continue;
        const Node* n = e.id ? t.find(e.id) : nullptr;
        VARIANT before, now;
        VariantInit(&before);
        VariantInit(&now);
        auto property = [&](PROPERTYID id) {
            if (wants(id) && a.raise_property) a.raise_property(p, id, before, now);
        };
        switch (e.kind) {
            case EventKind::kFocus:
                if (foreground() && a.raise_event) a.raise_event(p, UIA_AutomationFocusChangedEventId);
                break;
            case EventKind::kStructure:
                if (a.raise_structure) {
                    std::vector<int> rid = e.id ? runtime_id(e.id) : std::vector<int>();
                    a.raise_structure(p, StructureChangeType_ChildrenInvalidated, rid.empty() ? nullptr : rid.data(),
                                      static_cast<int>(rid.size()));
                }
                break;
            case EventKind::kName:
                set_bstr(&now, e.text);
                property(UIA_NamePropertyId);
                break;
            case EventKind::kValue:
                set_bstr(&now, e.text);
                property(UIA_ValueValuePropertyId);
                break;
            case EventKind::kToggle:
                if (n) set_i4(&now, n->has(kChecked) ? ToggleState_On : ToggleState_Off);
                property(UIA_ToggleToggleStatePropertyId);
                break;
            case EventKind::kExpandCollapse:
                property(UIA_ExpandCollapseExpandCollapseStatePropertyId);
                break;
            case EventKind::kRangeValue:
                if (n) {
                    now.vt = VT_R8;
                    now.dblVal = n->range.now;
                }
                property(UIA_RangeValueValuePropertyId);
                break;
            case EventKind::kEnabled:
                if (n) set_bool(&now, n->has(kEnabled));
                property(UIA_IsEnabledPropertyId);
                break;
            case EventKind::kSelected:
                if (a.raise_event) a.raise_event(p, UIA_SelectionItem_ElementSelectedEventId);
                break;
            case EventKind::kLiveRegion:
                if (a.raise_event) a.raise_event(p, UIA_LiveRegionChangedEventId);
                break;
            case EventKind::kTextChanged:
                if (a.raise_event) a.raise_event(p, UIA_Text_TextChangedEventId);
                break;
            case EventKind::kTextSelection:
                if (a.raise_event) a.raise_event(p, UIA_Text_TextSelectionChangedEventId);
                break;
            case EventKind::kNotification:
                if (a.raise_notification) {
                    BSTR text = bstr(e.text), activity = SysAllocString(L"android.pane");
                    a.raise_notification(p, NotificationKind_Other, NotificationProcessing_ImportantAll, text, activity);
                    SysFreeString(text);
                    SysFreeString(activity);
                }
                break;
            case EventKind::kGone: break;
        }
        VariantClear(&before);
        VariantClear(&now);
        p->Release();
    }
}

// Elements whose node is gone: out of the map, and disconnected so that clients let go of them.
void drop_gone_elements() {
    Host& h = host();
    std::vector<Element*> gone;
    {
        std::lock_guard<std::mutex> hold(h.m);
        for (auto it = h.elements.begin(); it != h.elements.end();) {
            if (!h.tree || !h.tree->find(it->first)) {
                gone.push_back(it->second);
                it = h.elements.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (Element* e : gone) {
        if (api().ok && api().disconnect) api().disconnect(static_cast<IRawElementProviderSimple*>(e));
        e->Release();
    }
}

void take_snapshot() {
    Host& h = host();
    std::wstring text;
    {
        std::lock_guard<std::mutex> hold(h.qm);
        text.swap(h.pending);
        h.pending_queued = false;
    }
    auto fresh = std::make_shared<Tree>();
    if (!parse(text, fresh.get())) return;
    std::shared_ptr<const Tree> before;
    {
        std::lock_guard<std::mutex> hold(h.m);
        if (!h.active || !h.hwnd) return;  // turned off while it waited
        before = h.tree;
        h.tree = fresh;
        h.frames_at_snapshot = h.frames;
        h.last_snapshot = now_ms();
        if (fresh->locale != h.lcid_of) {
            h.lcid_of = fresh->locale;
            h.lcid = fresh->locale.empty() ? 0 : LocaleNameToLCID(fresh->locale.c_str(), 0);
        }
    }
    h.tree_cv.notify_all();
    if (before && clients_listening()) raise(*fresh, diff(before.get(), *fresh));
    drop_gone_elements();
}

void turn_off() {
    Host& h = host();
    {
        std::lock_guard<std::mutex> hold(h.m);
        if (!h.active) return;
        h.active = false;
        h.tree.reset();
    }
    drop_gone_elements();
    AppSide* app;
    {
        std::lock_guard<std::mutex> hold(h.m);
        app = h.app;
    }
    if (app) app->activate(false);
}

void worker_loop() {
    Host& h = host();
    // UIA's raise functions and SAFEARRAYs want COM on the calling thread.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> hold(h.qm);
            h.qcv.wait_for(hold, std::chrono::seconds(5), [&] { return !h.jobs.empty(); });
            if (!h.jobs.empty()) {
                job = std::move(h.jobs.front());
                h.jobs.pop_front();
            }
        }
        if (job) {
            job();
            continue;
        }
        // Idle: a minute with no client call, while none listens for events, turns reading off.
        bool idle;
        {
            std::lock_guard<std::mutex> hold(h.m);
            idle = h.active && now_ms() - h.last_call > kIdleMs;
        }
        if (idle && !clients_listening()) turn_off();
    }
}

}  // namespace

// ---- The window's thread ------------------------------------------------------------------------------------

void attach(HWND hwnd, const Geometry& geometry) {
    Host& h = host();
    Root* old = nullptr;
    {
        std::lock_guard<std::mutex> hold(h.m);
        h.hwnd = hwnd;
        h.geometry = geometry;
        old = h.root;
        h.root = new Root;
    }
    if (old) old->Release();
}

bool on_get_object(HWND hwnd, WPARAM wp, LPARAM lp, LRESULT* result) {
    if (static_cast<LONG>(lp) != UiaRootObjectId) return false;
    Host& h = host();
    {
        std::lock_guard<std::mutex> hold(h.m);
        if (hwnd != h.hwnd || !h.root) return false;
    }
    const Api& a = api();
    if (!a.ok) return false;
    IRawElementProviderSimple* root = root_ref();
    if (!root) return false;
    *result = a.return_raw(hwnd, wp, lp, root);
    root->Release();
    return true;
}

void detach(HWND hwnd) {
    Host& h = host();
    Root* root = nullptr;
    bool was_active;
    {
        std::lock_guard<std::mutex> hold(h.m);
        if (hwnd != h.hwnd) return;
        h.hwnd = nullptr;
        root = h.root;
        h.root = nullptr;
        h.tree.reset();
        was_active = h.active;
        h.active = false;
    }
    // The documented way to let clients go of a window's providers: a null provider for it.
    if (g_api.ok) g_api.return_raw(hwnd, 0, 0, nullptr);
    if (root) {
        if (g_api.ok && g_api.disconnect) g_api.disconnect(root);
        root->Release();
    }
    queue(drop_gone_elements);
    if (was_active) ask_app([](AppSide* a) { a->activate(false); });
}

// ---- Any thread ---------------------------------------------------------------------------------------------

void set_app(AppSide* app) {
    std::lock_guard<std::mutex> hold(host().m);
    host().app = app;
}

void note_frame() {
    Host& h = host();
    ++h.frames;
    // While a client reads, a frame that no accessibility event explained asks for a snapshot.
    const int64_t now = now_ms();
    bool ask = false;
    {
        std::lock_guard<std::mutex> hold(h.m);
        if (h.active && now - h.last_call < kReadingMs && now - h.last_snapshot > kFrameRefreshMs &&
            now - h.last_refresh_ask > kFrameRefreshMs) {
            h.last_refresh_ask = now;
            ask = true;
        }
    }
    if (ask) ask_app([](AppSide* a) { a->refresh(); });
}

bool publish(std::wstring text) {
    Host& h = host();
    {
        std::lock_guard<std::mutex> hold(h.m);
        if (!h.active || !h.hwnd) return false;
    }
    bool start;
    {
        std::lock_guard<std::mutex> hold(h.qm);
        h.pending = std::move(text);  // a newer snapshot replaces one not yet parsed
        start = !h.pending_queued;
        h.pending_queued = true;
    }
    if (start) queue(take_snapshot);
    return true;
}

void announce(const std::wstring& text, bool assertive) {
    if (text.empty()) return;
    queue([text, assertive] {
        if (!clients_listening() || !g_api.raise_notification) return;
        IRawElementProviderSimple* root = root_ref();
        if (!root) return;
        BSTR said = bstr(text), activity = SysAllocString(L"android.announcement");
        g_api.raise_notification(root, NotificationKind_Other,
                                 assertive ? NotificationProcessing_ImportantMostRecent : NotificationProcessing_ImportantAll,
                                 said, activity);
        SysFreeString(said);
        SysFreeString(activity);
        root->Release();
    });
}

bool active() {
    std::lock_guard<std::mutex> hold(host().m);
    return host().active;
}

IUnknown* provider_for(HWND hwnd, int64_t id) {
    {
        std::lock_guard<std::mutex> hold(host().m);
        if (hwnd != host().hwnd) return nullptr;
    }
    touch();
    if (id == 0) return root_ref();
    snapshot(true);
    return element_ref(id);
}

void flush() {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    queue([&] {
        std::lock_guard<std::mutex> hold(m);
        done = true;
        cv.notify_one();
    });
    std::unique_lock<std::mutex> hold(m);
    cv.wait(hold, [&] { return done; });
}

}  // namespace winapk::host::uia
