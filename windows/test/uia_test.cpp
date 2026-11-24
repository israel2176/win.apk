// uia_test.cpp -- winapk-uia-test: what a screen reader sees of an app window
// (host/uia_provider.h), read through UI Automation's own client (IUIAutomation), as
// Narrator, NVDA or JAWS read it.
//
//   winapk-uia-test
//
// A window of its own thread stands in for the app's, and a fake app side publishes a view
// tree the way HostAccessibility.java does (Hebrew names, a right-to-left locale). Checked:
// nothing runs before a client asks (the app is not told, nothing is due); the tree's shape,
// names, control types, bounds, focus and culture; Invoke, Value, Toggle, RangeValue, Scroll
// and ScrollItem reaching the app as Android actions; a new snapshot replacing the old one;
// the snapshot parser's refusals. Exit code 0 when every check passes.

#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <uiautomation.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include "uia_provider.h"

namespace uia = winapk::host::uia;

namespace {

int g_failures = 0;

void check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// ---- The app's side -------------------------------------------------------------------------

// A DecorView with a heading, a button, a field, a check box, a seek bar and a scroll view, in
// Hebrew. Frame pixels are client pixels here (the geometry below is the identity).
std::wstring tree(const wchar_t* button_name, bool with_scroll) {
    std::wstring s = L"W1\t4\the-IL\t1\n";
    s += L"1\t0\tcom.android.internal.policy.DecorView\t\t\t\t257\t0,0,400,600\t2,3,4,5,6";
    s += with_scroll ? L",7" : L"";
    s += L"\t\t0\t\t0\n";
    s += L"2\t1\tandroid.widget.TextView\tשלום עולם\t\t\t1281\t"
         L"10,10,390,40\t\tapp:id/title\t256\t\t1\n";
    s += std::wstring(L"3\t1\tandroid.widget.Button\t") + button_name +
         L"\t\t\t267\t20,50,200,90\t\tapp:id/ok\t768\t\t0\n";
    s += L"4\t1\tandroid.widget.EditText\tשם\tabc\tהקלד\t327\t20,100,380,140\t\t"
         L"app:id/name\t384\t\t0\n";
    s += L"5\t1\tandroid.widget.CheckBox\tתזכורת\t\t\t315\t20,150,200,190\t\t\t768\t\t0\n";
    s += L"6\t1\tandroid.widget.SeekBar\tעוצמה\t\t\t8451\t20,200,380,230\t\t\t67\t"
         L"0,100,30\t0\n";
    if (with_scroll) s += L"7\t1\tandroid.widget.ScrollView\t\t\t\t771\t0,250,400,600\t\t\t265\t\t0\n";
    return s;
}

struct Performed {
    int64_t id = 0;
    int action = 0;
    std::wstring text;
};

class FakeApp : public uia::AppSide {
public:
    std::atomic<int> activations{0}, refreshes{0};
    std::wstring current = tree(L"אישור", true);

    void activate(bool on) override {
        if (on) {
            ++activations;
            uia::publish(current);
        }
    }
    void refresh() override {
        ++refreshes;
        uia::publish(current);
    }
    void perform(int64_t id, int action, const std::wstring& text) override {
        std::lock_guard<std::mutex> hold(lock_);
        last_ = Performed{id, action, text};
        ++count_;
        done_.notify_all();
    }
    // The next action after `before` actions, waited for a while.
    bool next(int before, Performed* out) {
        std::unique_lock<std::mutex> hold(lock_);
        if (!done_.wait_for(hold, std::chrono::seconds(5), [&] { return count_ > before; })) return false;
        *out = last_;
        return true;
    }
    int count() {
        std::lock_guard<std::mutex> hold(lock_);
        return count_;
    }

private:
    std::mutex lock_;
    std::condition_variable done_;
    Performed last_;
    int count_ = 0;
};

FakeApp g_app;

// ---- The window -----------------------------------------------------------------------------

HWND g_hwnd = nullptr;

bool to_screen(const RECT& f, RECT* out) {
    POINT a{f.left, f.top}, b{f.right, f.bottom};
    if (!ClientToScreen(g_hwnd, &a) || !ClientToScreen(g_hwnd, &b)) return false;
    *out = RECT{a.x, a.y, b.x, b.y};
    return true;
}

bool to_frame(POINT p, POINT* out) {
    if (!ScreenToClient(g_hwnd, &p)) return false;
    *out = p;
    return true;
}

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_GETOBJECT) {
        LRESULT r = 0;
        if (uia::on_get_object(hwnd, wp, lp, &r)) return r;
    }
    if (msg == WM_DESTROY) uia::detach(hwnd);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void window_thread(HANDLE ready) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WinApkUiaTest";
    RegisterClassW(&wc);
    RECT r{0, 0, 400, 600};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"Win.apk UIA test", WS_OVERLAPPEDWINDOW, 40, 40,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (g_hwnd) {
        uia::attach(g_hwnd, uia::Geometry{to_screen, to_frame});
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    }
    SetEvent(ready);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

// ---- The client side ------------------------------------------------------------------------

std::wstring name_of(IUIAutomationElement* e) {
    BSTR b = nullptr;
    std::wstring out;
    if (e && SUCCEEDED(e->get_CurrentName(&b)) && b) out.assign(b, SysStringLen(b));
    SysFreeString(b);
    return out;
}

CONTROLTYPEID type_of(IUIAutomationElement* e) {
    CONTROLTYPEID t = 0;
    if (e) e->get_CurrentControlType(&t);
    return t;
}

// An element's children, every one (a true condition: the raw view), as FindAll gives them.
std::vector<IUIAutomationElement*> children(IUIAutomation* client, IUIAutomationElement* parent) {
    std::vector<IUIAutomationElement*> out;
    IUIAutomationCondition* all = nullptr;
    if (!parent || FAILED(client->CreateTrueCondition(&all)) || !all) return out;
    IUIAutomationElementArray* found = nullptr;
    if (SUCCEEDED(parent->FindAll(TreeScope_Children, all, &found)) && found) {
        int n = 0;
        found->get_Length(&n);
        for (int i = 0; i < n; ++i) {
            IUIAutomationElement* e = nullptr;
            if (SUCCEEDED(found->GetElement(i, &e)) && e) out.push_back(e);
        }
        found->Release();
    }
    all->Release();
    return out;
}

// What a list of elements is, for a failed check: each one's control type, class and name.
void describe(const char* what, const std::vector<IUIAutomationElement*>& v) {
    std::printf("    %s: %zu element%s\n", what, v.size(), v.size() == 1 ? "" : "s");
    for (IUIAutomationElement* e : v) {
        BSTR cls = nullptr;
        e->get_CurrentClassName(&cls);
        std::printf("      type %d, class \"%ls\", name \"%ls\"\n", static_cast<int>(type_of(e)), cls ? cls : L"",
                    name_of(e).c_str());
        SysFreeString(cls);
    }
}

// The window's app content: its children less the ones Windows' own HWND providers add to a
// top-level window (the non-client area's title bar, with its system menu and buttons, and
// any menu or scroll bars). Real UI Automation merges those in beside the fragment root's
// children; Wine's has no non-client provider, so there the two lists are the same.
std::vector<IUIAutomationElement*> view_roots(IUIAutomation* client, IUIAutomationElement* window) {
    std::vector<IUIAutomationElement*> all = children(client, window), out;
    for (IUIAutomationElement* e : all) {
        const CONTROLTYPEID t = type_of(e);
        if (t == UIA_TitleBarControlTypeId || t == UIA_MenuBarControlTypeId || t == UIA_ScrollBarControlTypeId) {
            e->Release();
        } else {
            out.push_back(e);
        }
    }
    return out;
}

template <typename T>
T* pattern(IUIAutomationElement* e, PATTERNID id) {
    T* p = nullptr;
    if (e) e->GetCurrentPatternAs(id, __uuidof(T), reinterpret_cast<void**>(&p));
    return p;
}

void release_all(std::vector<IUIAutomationElement*>& v) {
    for (auto* e : v) e->Release();
    v.clear();
}

void parser_checks() {
    uia::Snapshot s;
    check("parser: refuses text that is not a snapshot", !uia::parse_snapshot(L"hello", &s));
    check("parser: refuses a node line with fields missing", !uia::parse_snapshot(L"W1\t0\t\t1\n1\t0\tX", &s));
    const bool ok = uia::parse_snapshot(tree(L"OK", true), &s);
    check("parser: reads the test tree", ok && s.nodes.size() == 7 && s.top.size() == 1 && s.focused == 4);
    const uia::Node* seek = ok ? s.find(6) : nullptr;
    check("parser: a SeekBar with a range it can set is a slider",
          seek && seek->control_type == UIA_SliderControlTypeId && seek->range_max == 100 && seek->range_now == 30);
    const uia::Node* title = ok ? s.find(2) : nullptr;
    check("parser: a heading's flags, its live region and its view id",
          title && (title->flags & uia::kHeading) && title->live == 1 && title->automation_id == L"app:id/title");
    check("parser: Hebrew's LCID from the locale", ok && s.lcid == 0x040D);
}

// A node's provider, as UIA holds it, asked directly.
template <typename T>
T* provider_pattern(int64_t id, PATTERNID pattern_id) {
    IUnknown* u = uia::provider_for(g_hwnd, id);
    IRawElementProviderSimple* simple = nullptr;
    T* out = nullptr;
    if (u && SUCCEEDED(u->QueryInterface(__uuidof(IRawElementProviderSimple), reinterpret_cast<void**>(&simple)))) {
        IUnknown* p = nullptr;
        if (SUCCEEDED(simple->GetPatternProvider(pattern_id, &p)) && p) {
            p->QueryInterface(__uuidof(T), reinterpret_cast<void**>(&out));
            p->Release();
        }
        simple->Release();
    }
    if (u) u->Release();
    return out;
}

VARIANT provider_property(int64_t id, PROPERTYID property) {
    VARIANT v;
    VariantInit(&v);
    IUnknown* u = uia::provider_for(g_hwnd, id);
    IRawElementProviderSimple* simple = nullptr;
    if (u && SUCCEEDED(u->QueryInterface(__uuidof(IRawElementProviderSimple), reinterpret_cast<void**>(&simple)))) {
        simple->GetPropertyValue(property, &v);
        simple->Release();
    }
    if (u) u->Release();
    return v;
}

std::wstring provider_name(IUnknown* u) {
    std::wstring out;
    IRawElementProviderSimple* simple = nullptr;
    if (u && SUCCEEDED(u->QueryInterface(__uuidof(IRawElementProviderSimple), reinterpret_cast<void**>(&simple)))) {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(simple->GetPropertyValue(UIA_NamePropertyId, &v)) && v.vt == VT_BSTR && v.bstrVal) {
            out.assign(v.bstrVal, SysStringLen(v.bstrVal));
        }
        VariantClear(&v);
        simple->Release();
    }
    return out;
}

// The patterns and properties as the providers give them to any client.
void provider_checks() {
    VARIANT v = provider_property(2, UIA_CulturePropertyId);
    check("provider: the views speak the app's language (Hebrew, 1037)", v.vt == VT_I4 && v.lVal == 0x040D);
    v = provider_property(2, UIA_HeadingLevelPropertyId);
    check("provider: a heading is a heading", v.vt == VT_I4 && v.lVal != 0);
    v = provider_property(2, UIA_LiveSettingPropertyId);
    check("provider: a polite live region is Polite", v.vt == VT_I4 && v.lVal == 1);
    v = provider_property(2, UIA_AutomationIdPropertyId);
    check("provider: a view's id is its AutomationId",
          v.vt == VT_BSTR && v.bstrVal && std::wstring(v.bstrVal) == L"app:id/title");
    VariantClear(&v);
    v = provider_property(4, UIA_HasKeyboardFocusPropertyId);
    check("provider: the focused field has the keyboard focus", v.vt == VT_BOOL && v.boolVal == VARIANT_TRUE);
    v = provider_property(2, UIA_IsOffscreenPropertyId);
    check("provider: a view the user sees is on screen", v.vt == VT_BOOL && v.boolVal == VARIANT_FALSE);
    check("provider: a TextView that does nothing cannot be invoked",
          provider_pattern<IInvokeProvider>(2, UIA_InvokePatternId) == nullptr);

    Performed p;
    int before = g_app.count();
    if (IInvokeProvider* invoke = provider_pattern<IInvokeProvider>(3, UIA_InvokePatternId)) {
        invoke->Invoke();
        invoke->Release();
        check("provider: Invoke clicks the button", g_app.next(before, &p) && p.id == 3 && p.action == uia::kActClick);
    } else {
        check("provider: a button can be invoked", false);
    }
    before = g_app.count();
    if (IValueProvider* value = provider_pattern<IValueProvider>(4, UIA_ValuePatternId)) {
        BSTR b = nullptr;
        value->get_Value(&b);
        check("provider: the field's value is its text", b && std::wstring(b) == L"abc");
        SysFreeString(b);
        value->SetValue(L"דנה");
        value->Release();
        check("provider: SetValue sets the field's Hebrew text",
              g_app.next(before, &p) && p.id == 4 && p.action == uia::kActSetText && p.text == L"דנה");
    } else {
        check("provider: a field has a value", false);
    }
    before = g_app.count();
    if (IToggleProvider* toggle = provider_pattern<IToggleProvider>(5, UIA_TogglePatternId)) {
        ToggleState state = ToggleState_Off;
        toggle->get_ToggleState(&state);
        check("provider: the check box is checked", state == ToggleState_On);
        toggle->Toggle();
        toggle->Release();
        check("provider: Toggle clicks the check box", g_app.next(before, &p) && p.id == 5 && p.action == uia::kActClick);
    } else {
        check("provider: a check box toggles", false);
    }
    before = g_app.count();
    if (IRangeValueProvider* range = provider_pattern<IRangeValueProvider>(6, UIA_RangeValuePatternId)) {
        double now = 0, max = 0;
        range->get_Value(&now);
        range->get_Maximum(&max);
        check("provider: the seek bar is at 30 of 100", now == 30 && max == 100);
        check("provider: a value past the range is refused", range->SetValue(101) == E_INVALIDARG);
        range->SetValue(55);
        range->Release();
        check("provider: SetValue sets the progress",
              g_app.next(before, &p) && p.id == 6 && p.action == uia::kActSetProgress && p.text == L"55");
    } else {
        check("provider: a seek bar has a range", false);
    }
    before = g_app.count();
    if (IScrollProvider* scroll = provider_pattern<IScrollProvider>(7, UIA_ScrollPatternId)) {
        BOOL vertical = FALSE, across = TRUE;
        scroll->get_VerticallyScrollable(&vertical);
        scroll->get_HorizontallyScrollable(&across);
        check("provider: a ScrollView scrolls vertically only", vertical && !across);
        scroll->Scroll(ScrollAmount_NoAmount, ScrollAmount_LargeIncrement);
        scroll->Release();
        check("provider: a page down scrolls it down (ACTION_SCROLL_DOWN)",
              g_app.next(before, &p) && p.id == 7 && p.action == uia::kActScrollDown);
    } else {
        check("provider: a scroll view scrolls", false);
    }
    before = g_app.count();
    if (IScrollItemProvider* item = provider_pattern<IScrollItemProvider>(2, UIA_ScrollItemPatternId)) {
        item->ScrollIntoView();
        item->Release();
        check("provider: ScrollIntoView shows the view on screen",
              g_app.next(before, &p) && p.id == 2 && p.action == uia::kActShowOnScreen);
    } else {
        check("provider: a view can be scrolled into view", false);
    }

    // The root: the element at a point, and the focus.
    IUnknown* u = uia::provider_for(g_hwnd, 0);
    IRawElementProviderFragmentRoot* root = nullptr;
    if (u && SUCCEEDED(u->QueryInterface(__uuidof(IRawElementProviderFragmentRoot), reinterpret_cast<void**>(&root)))) {
        POINT center{110, 70};
        ClientToScreen(g_hwnd, &center);
        IRawElementProviderFragment* at = nullptr;
        root->ElementProviderFromPoint(center.x, center.y, &at);
        check("provider: the element at the button's centre is the button",
              provider_name(at) == L"אישור");
        if (at) at->Release();
        IRawElementProviderFragment* focus = nullptr;
        root->GetFocus(&focus);
        check("provider: the root's focus is the focused field", provider_name(focus) == L"שם");
        if (focus) focus->Release();
        root->Release();
    } else {
        check("provider: the window's root is a fragment root", false);
    }
    if (u) u->Release();
}


}  // namespace

int main() {
    std::printf("winapk-uia-test\n");
    parser_checks();

    uia::set_app(&g_app);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread(window_thread, ready).detach();
    WaitForSingleObject(ready, 10000);
    if (!g_hwnd) {
        std::printf("  [FAIL] no window\n");
        return 1;
    }
    check("lazy: the app is not told before a client asks", g_app.activations.load() == 0);
    check("lazy: nothing is due before a client asks", !uia::active() && !uia::due());

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* client = nullptr;
    HRESULT h = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                 reinterpret_cast<void**>(&client));
    if (FAILED(h) || !client) {
        std::printf("  [FAIL] no UI Automation client (0x%08lx)\n", static_cast<unsigned long>(h));
        return 1;
    }
    IUIAutomationElement* window = nullptr;
    client->ElementFromHandle(g_hwnd, &window);
    check("the window is an element", window != nullptr);
    check("asking turned the app's accessibility on", g_app.activations.load() >= 1 && uia::active());

    std::vector<IUIAutomationElement*> top = view_roots(client, window);
    check("the window holds its one view root", top.size() == 1);
    if (top.size() != 1) {
        std::vector<IUIAutomationElement*> raw = children(client, window);
        describe("the window's children", raw);
        release_all(raw);
    }
    std::vector<IUIAutomationElement*> kids = top.empty() ? std::vector<IUIAutomationElement*>{}
                                                          : children(client, top[0]);
    check("the root holds its six views, in the app's order", kids.size() == 6);
    if (kids.size() != 6) describe("the root's children", kids);
    if (kids.size() == 6) {
        check("a heading's Hebrew text is its name", name_of(kids[0]) == L"שלום עולם");
        check("a TextView is Text", type_of(kids[0]) == UIA_TextControlTypeId);
        check("a Button is a button named in Hebrew",
              type_of(kids[1]) == UIA_ButtonControlTypeId && name_of(kids[1]) == L"אישור");
        check("an EditText is Edit", type_of(kids[2]) == UIA_EditControlTypeId);
        check("a CheckBox is a check box", type_of(kids[3]) == UIA_CheckBoxControlTypeId);
        check("a SeekBar is a slider", type_of(kids[4]) == UIA_SliderControlTypeId);
        check("a ScrollView is a pane", type_of(kids[5]) == UIA_PaneControlTypeId);

        RECT want{20, 50, 200, 90}, got{};
        to_screen(want, &want);
        kids[1]->get_CurrentBoundingRectangle(&got);
        check("the button's bounds are where it is drawn on the screen", EqualRect(&want, &got) != FALSE);


        // What Wine's client lacks (properties, patterns, ElementFromPoint) answers E_NOTIMPL
        // there: those checks are skipped, and provider_checks reads the same from the
        // providers themselves.
        VARIANT v;
        VariantInit(&v);
        if (kids[0]->GetCurrentPropertyValue(UIA_CulturePropertyId, &v) == E_NOTIMPL) {
            std::printf("  [SKIP] this UI Automation client reads no properties or patterns (Wine)\n");
        } else {
            check("client: the views speak the app's language (Hebrew, 1037)", v.vt == VT_I4 && v.lVal == 0x040D);
            VariantClear(&v);
            BOOL focus = FALSE;
            kids[2]->get_CurrentHasKeyboardFocus(&focus);
            check("client: the focused field has the keyboard focus", focus != FALSE);
            const int before = g_app.count();
            Performed p;
            IUIAutomationInvokePattern* invoke = pattern<IUIAutomationInvokePattern>(kids[1], UIA_InvokePatternId);
            check("client: a button can be invoked", invoke != nullptr);
            if (invoke) {
                invoke->Invoke();
                invoke->Release();
                check("client: Invoke clicks the view",
                      g_app.next(before, &p) && p.id == 3 && p.action == uia::kActClick);
            }
            IUIAutomationValuePattern* value = pattern<IUIAutomationValuePattern>(kids[2], UIA_ValuePatternId);
            check("client: a field has a value", value != nullptr);
            if (value) {
                BSTR b = nullptr;
                value->get_CurrentValue(&b);
                check("client: the field's value is its text", b && std::wstring(b) == L"abc");
                SysFreeString(b);
                value->Release();
            }
            IUIAutomationRangeValuePattern* range =
                pattern<IUIAutomationRangeValuePattern>(kids[4], UIA_RangeValuePatternId);
            check("client: a seek bar has a range", range != nullptr);
            if (range) {
                double now = 0;
                range->get_CurrentValue(&now);
                check("client: the seek bar is at 30", now == 30);
                range->Release();
            }
            IUIAutomationTogglePattern* toggle = pattern<IUIAutomationTogglePattern>(kids[3], UIA_TogglePatternId);
            check("client: a check box toggles", toggle != nullptr);
            if (toggle) toggle->Release();
            IUIAutomationScrollPattern* scroll = pattern<IUIAutomationScrollPattern>(kids[5], UIA_ScrollPatternId);
            check("client: a scroll view scrolls", scroll != nullptr);
            if (scroll) scroll->Release();
            POINT center{110, 70};
            ClientToScreen(g_hwnd, &center);
            if (WindowFromPoint(center) == g_hwnd) {
                IUIAutomationElement* at = nullptr;
                client->ElementFromPoint(center, &at);
                check("client: the element at the button's centre is the button",
                      name_of(at) == L"אישור");
                if (at) at->Release();
            } else {
                std::printf("  [SKIP] another window covers the test window: no point check\n");
            }
        }
    }
    release_all(kids);
    release_all(top);

    provider_checks();

    // The app draws a new screen: the button's name changes and the scroll view goes.
    g_app.current = tree(L"שמור", false);
    uia::publish(g_app.current);
    top = view_roots(client, window);
    kids = top.empty() ? std::vector<IUIAutomationElement*>{} : children(client, top[0]);
    check("a new snapshot replaces the old one", kids.size() == 5 && name_of(kids[1]) == L"שמור");
    if (kids.size() != 5) describe("the root's children after the new snapshot", kids);
    release_all(kids);
    release_all(top);

    if (window) window->Release();
    client->Release();
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ok", g_failures, g_failures == 1 ? "" : "s");
    std::fflush(stdout);
    // The provider's worker never ends; the test does.
    ExitProcess(g_failures ? 1 : 0);
}
