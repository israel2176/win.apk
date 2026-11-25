// uia_test.cpp -- the app window's UI Automation provider (host/uia_provider.h) on a real window.
//
// A window on its own thread answers WM_GETOBJECT as the app's does; a stand-in app records what
// the provider asks of it. The test publishes a snapshot (host/uia_tree.h) and then:
//   1. calls the providers directly, as UIA core would: laziness (nothing before the first call),
//      navigation, properties, every control pattern and what each sends to the app, the Text
//      pattern's ranges, hit-testing and focus, an element whose node is gone;
//   2. reads the window as a client does, through IUIAutomation: finds elements by name, reads a
//      value, invokes a pattern. Wine's UI Automation client lacks much of this; under Wine the
//      client's checks are reported and not counted (CI runs them on Windows).
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <uiautomation.h>

#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "uia_provider.h"

using namespace winapk::host;
using namespace winapk::host::uia;

namespace {

int g_failures = 0;
bool g_lenient = false;  // the client's checks under Wine

void check(bool ok, const char* what) {
    if (ok) return;
    if (g_lenient) {
        std::printf("  (Wine's client) not: %s\n", what);
        return;
    }
    std::printf("FAIL: %s\n", what);
    ++g_failures;
}

bool under_wine() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version");
}

// ---- The stand-in app --------------------------------------------------------------------------

struct Call {
    int64_t id;
    int action;
    std::wstring text;
    int a1, a2;
    double value;
};

class FakeApp final : public AppSide {
public:
    void activate(bool on) override {
        std::lock_guard<std::mutex> hold(m);
        (on ? activations : deactivations)++;
    }
    void refresh() override {
        std::lock_guard<std::mutex> hold(m);
        refreshes++;
    }
    void perform(int64_t id, int action, const std::wstring& text, int a1, int a2, double value) override {
        std::lock_guard<std::mutex> hold(m);
        calls.push_back(Call{id, action, text, a1, a2, value});
    }
    // The calls since the last take.
    std::vector<Call> take() {
        flush();
        std::lock_guard<std::mutex> hold(m);
        std::vector<Call> out;
        out.swap(calls);
        return out;
    }
    std::mutex m;
    int activations = 0, deactivations = 0, refreshes = 0;
    std::vector<Call> calls;
};

FakeApp g_app;

bool called(const std::vector<Call>& calls, int64_t id, int action) {
    for (const Call& c : calls) {
        if (c.id == id && c.action == action) return true;
    }
    return false;
}

// ---- Geometry: the frame drawn at (100, 200) on the screen, pixel for pixel -----------------------

bool to_screen(const Box& f, RECT* out) {
    *out = RECT{f.l + 100, f.t + 200, f.r + 100, f.b + 200};
    return true;
}

bool to_frame(POINT p, POINT* out) {
    *out = POINT{p.x - 100, p.y - 200};
    return true;
}

// ---- The snapshot ----------------------------------------------------------------------------------

Node node(int64_t id, int64_t parent, const wchar_t* cls, uint32_t flags, Box b) {
    Node n;
    n.id = id;
    n.parent = parent;
    n.class_name = cls;
    n.flags = flags | kEnabled | kVisible;
    n.bounds = b;
    return n;
}

void act(Node* n, int id) { n->action_ids.push_back(id); }

Tree screen(bool with_pears = true, bool focused_editor = true) {
    Tree t;
    t.focused = focused_editor ? 40 : 0;
    t.locale = L"he-IL";
    t.windows.push_back(Window{1, 1, L"Settings", true});
    Node root = node(1, 0, L"com.android.internal.policy.DecorView", 0, Box{0, 0, 720, 1280});
    root.children = {10, 20, 30, 40, 50, 60, 80, 90};
    Node heading = node(10, 1, L"android.widget.TextView", kHeading, Box{0, 0, 720, 60});
    heading.text = L"Network";
    heading.live = 1;
    Node row = node(20, 1, L"android.widget.LinearLayout", kClickable | kFocusable, Box{0, 60, 720, 160});
    act(&row, action::kClick);
    row.children = {21, 22, 23};
    Node title = node(21, 20, L"android.widget.TextView", 0, Box{0, 60, 300, 110});
    title.text = L"Wi-Fi";
    Node summary = node(22, 20, L"android.widget.TextView", 0, Box{0, 110, 300, 160});
    summary.text = L"Connected";
    Node sw = node(23, 20, L"android.widget.Switch", kCheckable | kChecked | kClickable | kFocusable,
                   Box{600, 80, 700, 140});
    act(&sw, action::kClick);
    Node label = node(30, 1, L"android.widget.TextView", 0, Box{0, 160, 720, 200});
    label.text = L"Name";
    Node edit = node(40, 1, L"android.widget.EditText", kEditable | kFocusable | kFocused | kClickable | kMultiLine,
                     Box{0, 200, 720, 300});
    edit.text = L"one two\nthree";
    edit.hint = L"Your name";
    edit.labeled_by = 30;
    edit.sel_start = 4;
    edit.sel_end = 7;
    edit.line_starts = {0, 8};
    edit.line_boxes = {Box{10, 210, 210, 250}, Box{10, 250, 160, 290}};
    for (int i = 0; i <= 13; ++i) edit.char_x.push_back(10 + 20 * (i < 8 ? i : i - 8));
    edit.word_breaks = {0, 3, 4, 7, 8, 13};
    for (int a : {action::kSetText, action::kSetSelection, action::kClick, action::kShowOnScreen}) act(&edit, a);
    Node seek = node(50, 1, L"android.widget.SeekBar", kFocusable, Box{0, 300, 720, 360});
    seek.range = RangeInfo{true, 0, 0, 100, 40};
    act(&seek, action::kSetProgress);
    Node list = node(60, 1, L"androidx.recyclerview.widget.RecyclerView", kScrollable, Box{0, 360, 720, 600});
    list.collection = CollectionInfo{true, 2, 1, 1, false};
    act(&list, action::kScrollForward);
    act(&list, action::kScrollDown);
    list.children = with_pears ? std::vector<int64_t>{61, 62} : std::vector<int64_t>{61};
    Node apples = node(61, 60, L"android.widget.FrameLayout", kClickable, Box{0, 360, 720, 480});
    apples.item = ItemInfo{true, 0, 1, 0, 1, false, true};
    apples.text = L"Apples";
    act(&apples, action::kClick);
    act(&apples, action::kShowOnScreen);
    Node pears = node(62, 60, L"android.widget.FrameLayout", kClickable, Box{0, 480, 720, 600});
    pears.item = ItemInfo{true, 1, 1, 0, 1, false, false};
    pears.text = L"Pears";
    act(&pears, action::kClick);
    Node more = node(80, 1, L"android.widget.ImageButton", kClickable, Box{0, 600, 100, 700});
    more.description = L"More";
    act(&more, action::kClick);
    act(&more, action::kExpand);
    Node off = node(90, 1, L"android.widget.Button", kClickable, Box{100, 600, 300, 700});
    off.flags &= ~kEnabled;
    off.text = L"Disabled";
    act(&off, action::kClick);
    t.nodes = {root, heading, row, title, summary, sw, label, edit, seek, list, apples};
    if (with_pears) t.nodes.push_back(pears);
    t.nodes.push_back(more);
    t.nodes.push_back(off);
    derive(&t);
    return t;
}

// ---- The window, on its own thread -----------------------------------------------------------------

HWND g_hwnd = nullptr;

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_GETOBJECT) {
        LRESULT r = 0;
        if (on_get_object(hwnd, wp, lp, &r)) return r;
    } else if (msg == WM_DESTROY) {
        detach(hwnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void window_thread(std::mutex* m, std::condition_variable* cv) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WinApkUiaTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"UIA test", WS_OVERLAPPEDWINDOW, 100, 100, 400, 600, nullptr,
                                nullptr, wc.hInstance, nullptr);
    attach(hwnd, Geometry{to_screen, to_frame});
    {
        std::lock_guard<std::mutex> hold(*m);
        g_hwnd = hwnd;
    }
    cv->notify_one();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// ---- Helpers over the providers ---------------------------------------------------------------------

template <typename T>
T* as(IUnknown* u, REFIID iid) {
    T* out = nullptr;
    if (u) u->QueryInterface(iid, reinterpret_cast<void**>(&out));
    return out;
}

IRawElementProviderSimple* element(int64_t id) {
    IUnknown* u = provider_for(g_hwnd, id);
    IRawElementProviderSimple* s = as<IRawElementProviderSimple>(u, IID_IRawElementProviderSimple);
    if (u) u->Release();
    return s;
}

std::wstring string_property(IRawElementProviderSimple* e, PROPERTYID p) {
    VARIANT v;
    VariantInit(&v);
    std::wstring out;
    if (e && SUCCEEDED(e->GetPropertyValue(p, &v)) && v.vt == VT_BSTR) out.assign(v.bstrVal, SysStringLen(v.bstrVal));
    VariantClear(&v);
    return out;
}

long long int_property(IRawElementProviderSimple* e, PROPERTYID p, long long absent = -999) {
    VARIANT v;
    VariantInit(&v);
    long long out = absent;
    if (e && SUCCEEDED(e->GetPropertyValue(p, &v))) {
        if (v.vt == VT_I4) out = v.lVal;
        if (v.vt == VT_BOOL) out = v.boolVal ? 1 : 0;
    }
    VariantClear(&v);
    return out;
}

template <typename T>
T* pattern(IRawElementProviderSimple* e, PATTERNID id, REFIID iid) {
    IUnknown* u = nullptr;
    if (!e || FAILED(e->GetPatternProvider(id, &u)) || !u) return nullptr;
    T* p = as<T>(u, iid);
    u->Release();
    return p;
}

int64_t id_of(IRawElementProviderFragment* f) {
    if (!f) return 0;
    SAFEARRAY* sa = nullptr;
    int64_t id = 0;
    if (SUCCEEDED(f->GetRuntimeId(&sa)) && sa) {
        LONG i = 1, j = 2;
        int hi = 0, lo = 0;
        SafeArrayGetElement(sa, &i, &hi);
        SafeArrayGetElement(sa, &j, &lo);
        id = (static_cast<int64_t>(static_cast<uint32_t>(hi)) << 32) | static_cast<uint32_t>(lo);
        SafeArrayDestroy(sa);
    }
    return id;
}

int64_t navigate(IRawElementProviderSimple* e, NavigateDirection d) {
    IRawElementProviderFragment* f = as<IRawElementProviderFragment>(e, IID_IRawElementProviderFragment);
    if (!f) return -1;
    IRawElementProviderFragment* to = nullptr;
    f->Navigate(d, &to);
    f->Release();
    if (!to) return 0;
    int64_t id = id_of(to);
    to->Release();
    return id;
}

std::wstring range_text(ITextRangeProvider* r) {
    BSTR b = nullptr;
    std::wstring out;
    if (r && SUCCEEDED(r->GetText(-1, &b)) && b) out.assign(b, SysStringLen(b));
    SysFreeString(b);
    return out;
}

// ---- 1. The providers -------------------------------------------------------------------------------

void test_lazy() {
    check(!active(), "nothing runs before a client asks");
    check(!publish(encode(screen())), "a snapshot before any client is refused");
    IUnknown* root = provider_for(g_hwnd, 0);
    check(root != nullptr, "the root provider");
    flush();
    check(active(), "a client's first call turns reading on");
    {
        std::lock_guard<std::mutex> hold(g_app.m);
        check(g_app.activations == 1, "and tells the app");
    }
    check(publish(encode(screen())), "now a snapshot is taken");
    flush();
    if (root) root->Release();
}

void test_navigation_and_properties() {
    IUnknown* r = provider_for(g_hwnd, 0);
    IRawElementProviderSimple* root = as<IRawElementProviderSimple>(r, IID_IRawElementProviderSimple);
    if (r) r->Release();
    check(navigate(root, NavigateDirection_FirstChild) == 1, "the root's first child is the window's root");
    if (root) root->Release();

    IRawElementProviderSimple* win = element(1);
    check(string_property(win, UIA_NamePropertyId) == L"Settings", "the window's root is named by its title");
    check(int_property(win, UIA_IsDialogPropertyId) == 0, "an activity's window is not a dialog");
    check(navigate(win, NavigateDirection_FirstChild) == 10 && navigate(win, NavigateDirection_LastChild) == 90,
          "the window's first and last children");
    if (win) win->Release();

    IRawElementProviderSimple* row = element(20);
    check(string_property(row, UIA_NamePropertyId) == L"Wi-Fi, Connected", "a row's name");
    check(int_property(row, UIA_ControlTypePropertyId) == UIA_ButtonControlTypeId, "a row is a button");
    check(navigate(row, NavigateDirection_Parent) == 1, "its parent");
    check(navigate(row, NavigateDirection_NextSibling) == 30 && navigate(row, NavigateDirection_PreviousSibling) == 10,
          "its siblings");
    IRawElementProviderFragment* f = as<IRawElementProviderFragment>(row, IID_IRawElementProviderFragment);
    UiaRect rect{};
    if (f) f->get_BoundingRectangle(&rect);
    check(rect.left == 100 && rect.top == 260 && rect.width == 720 && rect.height == 100,
          "its bounds on the screen, where the window draws the frame");
    if (f) f->Release();
    if (row) row->Release();

    IRawElementProviderSimple* title = element(21);
    check(int_property(title, UIA_IsControlElementPropertyId) == 0 && int_property(title, UIA_IsContentElementPropertyId) == 0,
          "a text its row speaks is in neither the control nor the content view");
    if (title) title->Release();

    IRawElementProviderSimple* heading = element(10);
    check(int_property(heading, UIA_HeadingLevelPropertyId) == 80052, "a heading");
    check(int_property(heading, UIA_LiveSettingPropertyId) == Polite, "a polite live region");
    check(int_property(heading, UIA_CulturePropertyId) == 0x040D, "the culture: Hebrew (Israel)");
    if (heading) heading->Release();

    IRawElementProviderSimple* edit = element(40);
    check(string_property(edit, UIA_NamePropertyId) == L"Name", "an editor is named by its label");
    check(string_property(edit, UIA_HelpTextPropertyId) == L"Your name", "its hint is its help text");
    VARIANT v;
    VariantInit(&v);
    bool labelled = false;
    if (edit && SUCCEEDED(edit->GetPropertyValue(UIA_LabeledByPropertyId, &v)) && v.vt == VT_UNKNOWN && v.punkVal) {
        IRawElementProviderFragment* lf = as<IRawElementProviderFragment>(v.punkVal, IID_IRawElementProviderFragment);
        labelled = id_of(lf) == 30;
        if (lf) lf->Release();
    }
    VariantClear(&v);
    check(labelled, "LabeledBy is the label's element");
    if (edit) edit->Release();

    IRawElementProviderSimple* pears = element(62);
    check(int_property(pears, UIA_PositionInSetPropertyId) == 2 && int_property(pears, UIA_SizeOfSetPropertyId) == 2,
          "a list's item: 2 of 2");
    if (pears) pears->Release();

    IRawElementProviderSimple* off = element(90);
    check(int_property(off, UIA_IsEnabledPropertyId) == 0, "a disabled button");
    IInvokeProvider* invoke = pattern<IInvokeProvider>(off, UIA_InvokePatternId, IID_IInvokeProvider);
    check(invoke && invoke->Invoke() == static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED), "invoking it is refused");
    if (invoke) invoke->Release();
    if (off) off->Release();
    g_app.take();
}

void test_patterns() {
    IRawElementProviderSimple* row = element(20);
    IInvokeProvider* invoke = pattern<IInvokeProvider>(row, UIA_InvokePatternId, IID_IInvokeProvider);
    check(invoke != nullptr, "a row invokes");
    if (invoke) invoke->Invoke();
    check(called(g_app.take(), 20, action::kClick), "Invoke clicks");
    check(pattern<IToggleProvider>(row, UIA_TogglePatternId, IID_IToggleProvider) == nullptr, "a row does not toggle");
    if (invoke) invoke->Release();
    if (row) row->Release();

    IRawElementProviderSimple* sw = element(23);
    IToggleProvider* toggle = pattern<IToggleProvider>(sw, UIA_TogglePatternId, IID_IToggleProvider);
    ToggleState state = ToggleState_Indeterminate;
    if (toggle) toggle->get_ToggleState(&state);
    check(state == ToggleState_On, "a checked switch is on");
    if (toggle) toggle->Toggle();
    check(called(g_app.take(), 23, action::kClick), "Toggle clicks");
    if (toggle) toggle->Release();
    if (sw) sw->Release();

    IRawElementProviderSimple* edit = element(40);
    IValueProvider* value = pattern<IValueProvider>(edit, UIA_ValuePatternId, IID_IValueProvider);
    BSTR b = nullptr;
    if (value) value->get_Value(&b);
    check(b && std::wstring(b) == L"one two\nthree", "an editor's value");
    SysFreeString(b);
    WINBOOL ro = TRUE;
    if (value) value->get_IsReadOnly(&ro);
    check(!ro, "it is not read-only");
    if (value) value->SetValue(L"hello");
    std::vector<Call> calls = g_app.take();
    check(calls.size() == 1 && calls[0].action == action::kSetText && calls[0].text == L"hello", "SetValue sets the text");
    if (value) value->Release();

    IRawElementProviderSimple* seek = element(50);
    IRangeValueProvider* range = pattern<IRangeValueProvider>(seek, UIA_RangeValuePatternId, IID_IRangeValueProvider);
    double now = 0, small = 0;
    if (range) {
        range->get_Value(&now);
        range->get_SmallChange(&small);
    }
    check(now == 40 && small == 5, "a seek bar's value, and a twentieth of its range as a step");
    check(range && range->SetValue(500) == E_INVALIDARG, "a value outside the range is refused");
    if (range) range->SetValue(60);
    calls = g_app.take();
    check(calls.size() == 1 && calls[0].action == action::kSetProgress && calls[0].value == 60, "SetValue sets the progress");
    if (range) range->Release();
    if (seek) seek->Release();

    IRawElementProviderSimple* list = element(60);
    IScrollProvider* scroll = pattern<IScrollProvider>(list, UIA_ScrollPatternId, IID_IScrollProvider);
    WINBOOL vert = FALSE, horiz = TRUE;
    if (scroll) {
        scroll->get_VerticallyScrollable(&vert);
        scroll->get_HorizontallyScrollable(&horiz);
    }
    check(vert && !horiz, "a list scrolls vertically");
    check(scroll && SUCCEEDED(scroll->Scroll(ScrollAmount_NoAmount, ScrollAmount_LargeIncrement)), "a page down");
    check(called(g_app.take(), 60, action::kScrollDown), "a page down is ACTION_SCROLL_DOWN");
    check(scroll && scroll->Scroll(ScrollAmount_NoAmount, ScrollAmount_SmallDecrement) ==
                        static_cast<HRESULT>(UIA_E_INVALIDOPERATION),
          "no way up: refused");
    if (scroll) scroll->Release();
    ISelectionProvider* selection = pattern<ISelectionProvider>(list, UIA_SelectionPatternId, IID_ISelectionProvider);
    SAFEARRAY* sa = nullptr;
    if (selection) selection->GetSelection(&sa);
    LONG lo = 0, hi = -1;
    if (sa) {
        SafeArrayGetLBound(sa, 1, &lo);
        SafeArrayGetUBound(sa, 1, &hi);
    }
    check(sa && hi - lo + 1 == 1, "the list's selection: one item");
    if (sa) SafeArrayDestroy(sa);
    if (selection) selection->Release();
    if (list) list->Release();

    IRawElementProviderSimple* apples = element(61);
    ISelectionItemProvider* item = pattern<ISelectionItemProvider>(apples, UIA_SelectionItemPatternId,
                                                                   IID_ISelectionItemProvider);
    WINBOOL selected = FALSE;
    if (item) item->get_IsSelected(&selected);
    check(selected, "the selected item");
    if (item) item->Release();
    IScrollItemProvider* into = pattern<IScrollItemProvider>(apples, UIA_ScrollItemPatternId, IID_IScrollItemProvider);
    if (into) into->ScrollIntoView();
    check(called(g_app.take(), 61, action::kShowOnScreen), "ScrollIntoView is ACTION_SHOW_ON_SCREEN");
    if (into) into->Release();
    if (apples) apples->Release();
    IRawElementProviderSimple* pears = element(62);
    item = pattern<ISelectionItemProvider>(pears, UIA_SelectionItemPatternId, IID_ISelectionItemProvider);
    if (item) item->Select();
    check(called(g_app.take(), 62, action::kClick), "selecting an item with no select action taps it");
    if (item) item->Release();
    if (pears) pears->Release();

    IRawElementProviderSimple* more = element(80);
    IExpandCollapseProvider* ec = pattern<IExpandCollapseProvider>(more, UIA_ExpandCollapsePatternId,
                                                                   IID_IExpandCollapseProvider);
    ExpandCollapseState st = ExpandCollapseState_LeafNode;
    if (ec) ec->get_ExpandCollapseState(&st);
    check(st == ExpandCollapseState_Collapsed, "a node that can expand is collapsed");
    if (ec) ec->Expand();
    check(called(g_app.take(), 80, action::kExpand), "Expand is ACTION_EXPAND");
    check(ec && ec->Collapse() == static_cast<HRESULT>(UIA_E_INVALIDOPERATION), "it cannot collapse");
    if (ec) ec->Release();
    if (more) more->Release();
    if (edit) edit->Release();
}

void test_text() {
    IRawElementProviderSimple* edit = element(40);
    ITextProvider2* text = pattern<ITextProvider2>(edit, UIA_TextPattern2Id, IID_ITextProvider2);
    check(text != nullptr, "an editor has the Text pattern");
    if (!text) {
        if (edit) edit->Release();
        return;
    }
    ITextRangeProvider* doc = nullptr;
    text->get_DocumentRange(&doc);
    check(range_text(doc) == L"one two\nthree", "the document range");
    ITextRangeProvider* r = nullptr;
    if (doc) doc->Clone(&r);
    if (r) {
        r->MoveEndpointByRange(TextPatternRangeEndpoint_End, r, TextPatternRangeEndpoint_Start);
        int moved = 0;
        r->Move(TextUnit_Word, 1, &moved);
        r->ExpandToEnclosingUnit(TextUnit_Word);
        check(moved == 1 && range_text(r) == L"two\n", "the second word");
        r->ExpandToEnclosingUnit(TextUnit_Line);
        check(range_text(r) == L"one two\n", "its line");
        r->Move(TextUnit_Line, 1, &moved);
        check(moved == 1 && range_text(r) == L"three", "the next line");
        SAFEARRAY* rects = nullptr;
        r->GetBoundingRectangles(&rects);
        LONG hi = -1;
        double x = 0, y = 0;
        if (rects) {
            SafeArrayGetUBound(rects, 1, &hi);
            LONG i0 = 0, i1 = 1;
            SafeArrayGetElement(rects, &i0, &x);
            SafeArrayGetElement(rects, &i1, &y);
            SafeArrayDestroy(rects);
        }
        check(hi == 3 && x == 110 && y == 450, "the line's rectangle on the screen");
        r->Select();
        std::vector<Call> calls = g_app.take();
        check(calls.size() == 1 && calls[0].action == action::kSetSelection && calls[0].a1 == 8 && calls[0].a2 == 13,
              "Select sets the selection");
        ITextRangeProvider* found = nullptr;
        doc->FindText(SysAllocString(L"TWO"), FALSE, TRUE, &found);
        int cmp = -1;
        if (found) found->CompareEndpoints(TextPatternRangeEndpoint_Start, doc, TextPatternRangeEndpoint_Start, &cmp);
        check(found && cmp == 4, "FindText, ignoring case");
        if (found) found->Release();
        VARIANT v;
        VariantInit(&v);
        r->GetAttributeValue(40015, &v);  // UIA_IsReadOnlyAttributeId
        check(v.vt == VT_BOOL && v.boolVal == VARIANT_FALSE, "an editor's text is not read-only");
        VariantClear(&v);
        r->Release();
    }
    SAFEARRAY* sel = nullptr;
    text->GetSelection(&sel);
    ITextRangeProvider* s = nullptr;
    if (sel) {
        LONG i = 0;
        SafeArrayGetElement(sel, &i, &s);
        SafeArrayDestroy(sel);
    }
    check(range_text(s) == L"two", "the selection");
    if (s) s->Release();
    WINBOOL caret_active = FALSE;
    ITextRangeProvider* caret = nullptr;
    text->GetCaretRange(&caret_active, &caret);
    int at = -1;
    if (caret) caret->CompareEndpoints(TextPatternRangeEndpoint_Start, doc, TextPatternRangeEndpoint_Start, &at);
    check(caret && caret_active && at == 7, "the caret at the selection's end, active in the focused editor");
    if (caret) caret->Release();
    ITextRangeProvider* p = nullptr;
    text->RangeFromPoint(UiaPoint{100 + 51, 200 + 260}, &p);
    at = -1;
    if (p) p->CompareEndpoints(TextPatternRangeEndpoint_Start, doc, TextPatternRangeEndpoint_Start, &at);
    check(at == 10, "the offset at a point on the second line");
    if (p) p->Release();
    if (doc) doc->Release();
    text->Release();
    if (edit) edit->Release();
}

void test_hit_and_focus() {
    IUnknown* r = provider_for(g_hwnd, 0);
    IRawElementProviderFragmentRoot* root = as<IRawElementProviderFragmentRoot>(r, IID_IRawElementProviderFragmentRoot);
    if (r) r->Release();
    IRawElementProviderFragment* hit = nullptr;
    if (root) root->ElementProviderFromPoint(100 + 650, 200 + 100, &hit);
    check(id_of(hit) == 23, "the switch is under its point, over its row");
    if (hit) hit->Release();
    hit = nullptr;
    if (root) root->ElementProviderFromPoint(100 + 10, 200 + 400, &hit);
    check(id_of(hit) == 61, "an item under its point");
    if (hit) hit->Release();
    IRawElementProviderFragment* focus = nullptr;
    if (root) root->GetFocus(&focus);
    check(id_of(focus) == 40, "the focused editor");
    if (focus) focus->Release();
    if (root) root->Release();
}

void test_gone() {
    IRawElementProviderSimple* pears = element(62);
    check(pears != nullptr, "an item's provider");
    publish(encode(screen(false)));
    flush();
    VARIANT v;
    VariantInit(&v);
    check(pears && pears->GetPropertyValue(UIA_NamePropertyId, &v) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE),
          "once its node is gone, its provider says so");
    check(element(62) == nullptr, "and no new one is made for it");
    if (pears) pears->Release();
    IRawElementProviderSimple* list = element(60);
    check(navigate(list, NavigateDirection_LastChild) == 61, "the list's children follow the snapshot");
    if (list) list->Release();
    publish(encode(screen()));
    flush();
}

// ---- 2. A client ---------------------------------------------------------------------------------

void test_client() {
    IUIAutomation* ua = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
                                reinterpret_cast<void**>(&ua))) || !ua) {
        check(false, "a UI Automation client");
        return;
    }
    IUIAutomationElement* window = nullptr;
    ua->ElementFromHandle(g_hwnd, &window);
    check(window != nullptr, "the window's element");
    // By name and control type (a label and the editor it names share a name).
    auto find = [&](const wchar_t* name, CONTROLTYPEID type) -> IUIAutomationElement* {
        IUIAutomationElement* out = nullptr;
        if (!window) return out;
        VARIANT v, t;
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(name);
        t.vt = VT_I4;
        t.lVal = type;
        IUIAutomationCondition *by_name = nullptr, *by_type = nullptr, *both = nullptr;
        ua->CreatePropertyCondition(UIA_NamePropertyId, v, &by_name);
        ua->CreatePropertyCondition(UIA_ControlTypePropertyId, t, &by_type);
        if (by_name && by_type) ua->CreateAndCondition(by_name, by_type, &both);
        if (both) window->FindFirst(TreeScope_Descendants, both, &out);
        for (IUIAutomationCondition* c : {by_name, by_type, both}) {
            if (c) c->Release();
        }
        VariantClear(&v);
        return out;
    };
    // Real Windows puts the title bar's elements beside ours; find by name, not by position.
    IUIAutomationElement* row = find(L"Wi-Fi, Connected", UIA_ButtonControlTypeId);
    check(row != nullptr, "the client finds the row by its name");
    CONTROLTYPEID ct = 0;
    if (row) row->get_CurrentControlType(&ct);
    check(ct == UIA_ButtonControlTypeId, "the client sees a button");
    IUIAutomationInvokePattern* invoke = nullptr;
    if (row) row->GetCurrentPatternAs(UIA_InvokePatternId, IID_IUIAutomationInvokePattern, reinterpret_cast<void**>(&invoke));
    if (invoke) invoke->Invoke();
    check(called(g_app.take(), 20, action::kClick), "the client's Invoke reaches the app");
    if (invoke) invoke->Release();
    if (row) row->Release();
    IUIAutomationElement* edit = find(L"Name", UIA_EditControlTypeId);
    IUIAutomationValuePattern* value = nullptr;
    if (edit) edit->GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern, reinterpret_cast<void**>(&value));
    BSTR b = nullptr;
    if (value) value->get_CurrentValue(&b);
    check(b && std::wstring(b) == L"one two\nthree", "the client reads the editor's value");
    SysFreeString(b);
    if (value) value->Release();
    if (edit) edit->Release();
    if (window) window->Release();
    ua->Release();
}

}  // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    set_app(&g_app);
    std::mutex m;
    std::condition_variable cv;
    std::thread t(window_thread, &m, &cv);
    {
        std::unique_lock<std::mutex> hold(m);
        cv.wait(hold, [] { return g_hwnd != nullptr; });
    }
    test_lazy();
    test_navigation_and_properties();
    test_patterns();
    test_text();
    test_hit_and_focus();
    test_gone();
    g_lenient = under_wine();
    test_client();
    g_lenient = false;
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    t.join();
    flush();
    check(!active(), "a destroyed window is no longer read");
    {
        std::lock_guard<std::mutex> hold(g_app.m);
        check(g_app.deactivations == 1, "and the app is told");
    }
    CoUninitialize();
    if (g_failures) {
        std::printf("%d FAILED\n", g_failures);
        return 1;
    }
    std::printf("uia: ALL PASSED\n");
    return 0;
}
