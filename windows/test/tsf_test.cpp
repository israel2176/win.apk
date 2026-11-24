// tsf_test.cpp -- the focused editor as a TSF text store (host/tsf_store.h), driven as TSF's
// manager drives one: a stand-in ITextStoreACPSink takes the locks and makes the calls a text
// service's edit session makes, and a stand-in composition view stands for TSF's ranges. Checks
// the lock rules (one lock at a time, TS_E_SYNCHRONOUS, the asynchronous queue and a read
// lock's way to write, TS_E_NOLOCK), one edit per write lock for the editor, compositions,
// reconversion, a reversed selection, the editor's own changes as notifications outside locks
// and never for the text services' own edits, a stale copy dropped, GetTextExt and
// GetACPFromPoint from the editor's character boxes (TS_E_NOLAYOUT while the edit is on its
// way), a password's read-only empty document, the input scope attribute and GetFormattedText.
// Then the session on the system's TSF, where there is one (Windows; Wine 9's is partial).
//
// Exits 0 when every check passes.

#include "tsf_session.h"
#include "tsf_store.h"

#include <inputscope.h>
#include <olectl.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace winapk::host;
using textinput::Box;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

// ---- stand-ins --------------------------------------------------------------------------------

class Sink final : public ITextStoreACPSink {
public:
    std::function<void(DWORD)> on_lock;
    std::vector<TS_TEXTCHANGE> text_changes;
    int selection_changes = 0, layout_changes = 0;
    ITextStoreACP* store = nullptr;
    bool saw_lock_in_notification = false;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_ITextStoreACPSink) {
            *out = static_cast<ITextStoreACPSink*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE OnTextChange(DWORD, const TS_TEXTCHANGE* c) override {
        text_changes.push_back(*c);
        probe_lock();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSelectionChange() override {
        selection_changes++;
        probe_lock();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnLayoutChange(TsLayoutCode, TsViewCookie) override {
        layout_changes++;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStatusChange(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnAttrsChange(LONG, LONG, ULONG, const TS_ATTRID*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnLockGranted(DWORD flags) override {
        // A copy: the lock's body may set the next lock's (a queued request).
        const std::function<void(DWORD)> body = on_lock;
        if (body) body(flags);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStartEditTransaction() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnEndEditTransaction() override { return S_OK; }
    void clear() {
        text_changes.clear();
        selection_changes = layout_changes = 0;
    }
    LONG refs = 1;

private:
    // A notification never comes inside a lock: the store's methods refuse without one.
    void probe_lock() {
        LONG end = 0;
        if (store && store->GetEndACP(&end) != TS_E_NOLOCK) saw_lock_in_notification = true;
    }
};

// A composition's range as TSF's, standing for ITfRangeACP: only its extent is asked.
class Range final : public ITfRangeACP {
public:
    LONG start = 0, end = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_ITfRange || riid == IID_ITfRangeACP) {
            *out = static_cast<ITfRangeACP*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetExtent(LONG* a, LONG* n) override {
        *a = start;
        *n = end - start;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetExtent(LONG, LONG) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetText(TfEditCookie, DWORD, WCHAR*, ULONG, ULONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetText(TfEditCookie, DWORD, const WCHAR*, LONG) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFormattedText(TfEditCookie, IDataObject**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetEmbedded(TfEditCookie, REFGUID, REFIID, IUnknown**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE InsertEmbedded(TfEditCookie, DWORD, IDataObject*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShiftStart(TfEditCookie, LONG, LONG*, const TF_HALTCOND*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShiftEnd(TfEditCookie, LONG, LONG*, const TF_HALTCOND*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShiftStartToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShiftEndToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShiftStartRegion(TfEditCookie, TfShiftDir, WINBOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ShiftEndRegion(TfEditCookie, TfShiftDir, WINBOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsEmpty(TfEditCookie, WINBOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Collapse(TfEditCookie, TfAnchor) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsEqualStart(TfEditCookie, ITfRange*, TfAnchor, WINBOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsEqualEnd(TfEditCookie, ITfRange*, TfAnchor, WINBOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CompareStart(TfEditCookie, ITfRange*, TfAnchor, LONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CompareEnd(TfEditCookie, ITfRange*, TfAnchor, LONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AdjustForInsert(TfEditCookie, ULONG, WINBOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetGravity(TfGravity*, TfGravity*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetGravity(TfEditCookie, TfGravity, TfGravity) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Clone(ITfRange**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetContext(ITfContext**) override { return E_NOTIMPL; }
};

class View final : public ITfCompositionView {
public:
    Range range;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_ITfCompositionView) {
            *out = static_cast<ITfCompositionView*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetOwnerClsid(CLSID* c) override {
        *c = CLSID_NULL;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRange(ITfRange** r) override {
        *r = &range;
        return S_OK;
    }
};

// The window: frame pixels are screen pixels moved by (1000, 500).
class Owner final : public tsf::StoreOwner {
public:
    std::vector<textinput::Edit> edits;
    HWND window() override { return nullptr; }
    void edit(const textinput::Edit& e) override { edits.push_back(e); }
    bool to_screen(const Box& f, RECT* s) override {
        *s = RECT{static_cast<LONG>(f.left) + 1000, static_cast<LONG>(f.top) + 500, static_cast<LONG>(f.right) + 1000,
                  static_cast<LONG>(f.bottom) + 500};
        return true;
    }
    bool from_screen(POINT p, float* x, float* y) override {
        *x = static_cast<float>(p.x - 1000);
        *y = static_cast<float>(p.y - 500);
        return true;
    }
    uint32_t sys_color(int) override { return 0xff336699u; }
};

textinput::Snapshot snap(int epoch, int acked, const std::u16string& text, int anchor, int caret) {
    textinput::Snapshot s;
    s.epoch = epoch;
    s.acked = acked;
    s.text = text;
    s.anchor = anchor;
    s.caret = caret;
    return s;
}

std::u16string text_of(ITextStoreACP* store) {
    WCHAR buf[256] = {};
    ULONG got = 0, runs = 0;
    LONG next = 0;
    TS_RUNINFO run{};
    if (FAILED(store->GetText(0, -1, buf, 256, &got, &run, 1, &runs, &next))) return u"<error>";
    return std::u16string(reinterpret_cast<const char16_t*>(buf), got);
}

// Takes a lock of `flags` and runs `body` in it.
HRESULT lock(tsf::Store* store, Sink& sink, DWORD flags, const std::function<void(DWORD)>& body) {
    sink.on_lock = body;
    HRESULT session = E_FAIL;
    const HRESULT hr = store->RequestLock(flags, &session);
    sink.on_lock = nullptr;
    return FAILED(hr) ? hr : session;
}

tsf::Store* make(Owner& owner, Sink& sink, const textinput::FieldPolicy& policy, const textinput::Snapshot& s) {
    auto* store = new tsf::Store(&owner, policy, s);
    ITextStoreACP* acp = store;
    acp->AdviseSink(IID_ITextStoreACPSink, &sink, TS_AS_ALL_SINKS);
    sink.store = acp;
    return store;
}

// ---- the checks -------------------------------------------------------------------------------

void interfaces() {
    Owner owner;
    Sink sink;
    tsf::Store* store = make(owner, sink, textinput::field_policy(1, 0), snap(1, 0, u"abc", 3, 3));
    ITextStoreACP* acp = store;
    void* p = nullptr;
    check(SUCCEEDED(acp->QueryInterface(tsf::kIID_ITextStoreACP2, &p)) && p, "ITextStoreACP2 is there");
    if (p) static_cast<IUnknown*>(p)->Release();
    p = nullptr;
    check(SUCCEEDED(acp->QueryInterface(IID_ITfContextOwnerCompositionSink, &p)) && p,
          "the composition sink is there");
    if (p) static_cast<IUnknown*>(p)->Release();
    p = nullptr;
    check(SUCCEEDED(acp->QueryInterface(IID_ITfTextEditSink, &p)) && p, "the text edit sink is there");
    if (p) static_cast<IUnknown*>(p)->Release();
    Sink other;
    check(acp->AdviseSink(IID_ITextStoreACPSink, &other, TS_AS_ALL_SINKS) == CONNECT_E_ADVISELIMIT,
          "a second sink is refused");
    TS_STATUS status{};
    acp->GetStatus(&status);
    check((status.dwDynamicFlags & TS_SD_INPUTPANEMANUALDISPLAYENABLE) && !(status.dwDynamicFlags & TS_SD_READONLY) &&
              (status.dwStaticFlags & TS_SS_TKBPREDICTIONENABLE) && !(status.dwStaticFlags & TS_SS_TKBAUTOCORRECTENABLE),
          "a text field: writable, the app shows the touch keyboard, suggestions on");
    check(acp->UnadviseSink(&sink) == S_OK, "the sink goes");
    acp->Release();
}

void locks() {
    Owner owner;
    Sink sink;
    tsf::Store* store = make(owner, sink, textinput::field_policy(1, 0), snap(1, 0, u"hello", 5, 5));
    ITextStoreACP* acp = store;
    LONG end = 0;
    check(acp->GetEndACP(&end) == TS_E_NOLOCK, "no lock, no reading");
    HRESULT inner = S_OK, write_in_read = S_OK, granted_order = S_OK;
    std::vector<DWORD> grants;
    const HRESULT hr = lock(store, sink, TS_LF_READ | TS_LF_SYNC, [&](DWORD flags) {
        grants.push_back(flags);
        check(acp->GetEndACP(&end) == S_OK && end == 5, "a read lock reads");
        write_in_read = acp->SetText(0, 0, 0, L"x", 1, nullptr);
        HRESULT s = S_OK;
        acp->RequestLock(TS_LF_READWRITE | TS_LF_SYNC, &s);
        inner = s;
        // The read lock's holder asks to write, later: queued, granted when this lock ends.
        sink.on_lock = [&](DWORD f) {
            grants.push_back(f);
            granted_order = acp->SetText(0, 5, 5, L"!", 1, nullptr);
        };
        acp->RequestLock(TS_LF_READWRITE, &s);
        check(s == TS_S_ASYNC, "an asynchronous request inside a lock is queued (TS_S_ASYNC)");
    });
    check(hr == S_OK, "a synchronous read lock is granted at once");
    check(write_in_read == TS_E_NOLOCK, "a read lock does not write");
    check(inner == TS_E_SYNCHRONOUS, "a synchronous request inside a lock fails (TS_E_SYNCHRONOUS)");
    check(grants.size() == 2 && grants[0] == TS_LF_READ && grants[1] == TS_LF_READWRITE && granted_order == S_OK,
          "the queued write lock is granted after the read lock ends, and writes");
    check(text_of(acp) == u"<error>", "the lock is gone after it was granted");
    check(owner.edits.size() == 1 && owner.edits[0].text == u"!" && owner.edits[0].start == 5,
          "the write lock's change is one edit for the editor");
    check(sink.text_changes.empty(), "no notification of the text services' own change");
    acp->Release();
}

void edits() {
    Owner owner;
    Sink sink;
    tsf::Store* store = make(owner, sink, textinput::field_policy(1, 0), snap(1, 0, u"ab", 2, 2));
    ITextStoreACP* acp = store;
    // Two writes in one lock: one edit.
    lock(store, sink, TS_LF_READWRITE | TS_LF_SYNC, [&](DWORD) {
        TS_TEXTCHANGE c{};
        acp->SetText(0, 0, 1, L"A", 1, &c);
        check(c.acpStart == 0 && c.acpOldEnd == 1 && c.acpNewEnd == 1, "SetText reports its change");
        LONG s = 0, e = 0;
        acp->InsertTextAtSelection(TS_IAS_QUERYONLY, L"cd", 2, &s, &e, nullptr);
        check(s == 2 && e == 4 && text_of(acp) == u"Ab", "a query inserts nothing");
        acp->InsertTextAtSelection(0, L"cd", 2, &s, &e, &c);
        check(s == 2 && e == 4 && text_of(acp) == u"Abcd", "InsertTextAtSelection inserts at the selection");
        TS_SELECTION_ACP sel{};
        ULONG got = 0;
        acp->GetSelection(TS_DEFAULT_SELECTION, 1, &sel, &got);
        check(got == 1 && sel.acpStart == 4 && sel.acpEnd == 4, "and leaves the caret after it");
        check(acp->SetText(0, 3, 9, L"x", 1, nullptr) == TS_E_INVALIDPOS, "a range past the end is refused");
    });
    check(owner.edits.size() == 1 && owner.edits[0].start == 0 && owner.edits[0].end == 2 &&
              owner.edits[0].text == u"Abcd" && owner.edits[0].caret == 4 && owner.edits[0].seq == 1,
          "two writes in a lock are one edit");
    // A reversed selection: the caret at its start, as Android's selection end.
    lock(store, sink, TS_LF_READWRITE | TS_LF_SYNC, [&](DWORD) {
        TS_SELECTION_ACP sel{};
        sel.acpStart = 1;
        sel.acpEnd = 3;
        sel.style.ase = TS_AE_START;
        acp->SetSelection(1, &sel);
    });
    check(owner.edits.size() == 2 && owner.edits[1].anchor == 3 && owner.edits[1].caret == 1 &&
              owner.edits[1].text.empty(),
          "a selection made backwards reaches the editor backwards, nothing replaced");
    lock(store, sink, TS_LF_READ | TS_LF_SYNC, [&](DWORD) {
        TS_SELECTION_ACP sel{};
        ULONG got = 0;
        acp->GetSelection(0, 1, &sel, &got);
        check(sel.style.ase == TS_AE_START, "and reads back so");
        // GetText's limits: part of the text, the run info, the next position.
        WCHAR buf[2] = {};
        ULONG n = 0, runs = 0;
        LONG next = 0;
        TS_RUNINFO run{};
        acp->GetText(1, -1, buf, 2, &n, &run, 1, &runs, &next);
        check(n == 2 && buf[0] == L'b' && runs == 1 && run.uCount == 2 && run.type == TS_RT_PLAIN && next == 3,
              "GetText stops at the buffer's size");
        check(acp->GetText(5, 9, buf, 2, &n, &run, 1, &runs, &next) == TS_E_INVALIDPOS, "and past the end fails");
    });
    acp->Release();
}

void compositions() {
    Owner owner;
    Sink sink;
    tsf::Store* store = make(owner, sink, textinput::field_policy(1, 0), snap(4, 0, u"ab", 2, 2));
    ITextStoreACP* acp = store;
    ITfContextOwnerCompositionSink* comp = store;
    View view;
    // A Japanese composition: started, written, grown, converted, committed.
    lock(store, sink, TS_LF_READWRITE | TS_LF_SYNC, [&](DWORD) {
        BOOL ok = FALSE;
        view.range.start = view.range.end = 2;
        comp->OnStartComposition(&view, &ok);
        check(ok == TRUE, "a text field takes a composition");
        acp->SetText(0, 2, 2, L"に", 1, nullptr);  // に
        view.range.end = 3;
        comp->OnUpdateComposition(&view, &view.range);
        TS_SELECTION_ACP sel{3, 3, {TS_AE_NONE, FALSE}};
        acp->SetSelection(1, &sel);
    });
    check(owner.edits.size() == 1 && owner.edits[0].comp_start == 2 && owner.edits[0].comp_end == 3 &&
              owner.edits[0].text == u"に",
          "a composition's first lock: its text, composing");
    lock(store, sink, TS_LF_READWRITE | TS_LF_SYNC, [&](DWORD) {
        acp->SetText(0, 2, 3, L"日本", 2, nullptr);  // 日本
        view.range.end = 4;
        TS_SELECTION_ACP sel{4, 4, {TS_AE_NONE, FALSE}};
        acp->SetSelection(1, &sel);
    });
    check(owner.edits.size() == 2 && owner.edits[1].start == 2 && owner.edits[1].end == 3 &&
              owner.edits[1].comp_end == 4 && owner.edits[1].text == u"日本",
          "a conversion: the composition read from its view at the lock's end");
    lock(store, sink, TS_LF_READWRITE | TS_LF_SYNC, [&](DWORD) { comp->OnEndComposition(&view); });
    check(owner.edits.size() == 3 && owner.edits[2].comp_start == -1 && owner.edits[2].text.empty(),
          "its end commits it: nothing replaced, no composition");
    // A composition ended by TSF outside a lock (the context going) still reaches the editor.
    lock(store, sink, TS_LF_READWRITE | TS_LF_SYNC, [&](DWORD) {
        BOOL ok = FALSE;
        view.range.start = 0;
        view.range.end = 2;
        comp->OnStartComposition(&view, &ok);  // reconversion: "ab" composed again, unchanged
    });
    check(owner.edits.size() == 4 && owner.edits[3].start == 0 && owner.edits[3].end == 2 &&
              owner.edits[3].text == u"ab" && owner.edits[3].comp_start == 0 && owner.edits[3].comp_end == 2,
          "reconversion makes committed text the composition, in place");
    comp->OnEndComposition(&view);
    check(owner.edits.size() == 5 && owner.edits[4].comp_start == -1, "a composition ended outside a lock");
    acp->Release();
}

void editor_changes() {
    Owner owner;
    Sink sink;
    tsf::Store* store = make(owner, sink, textinput::field_policy(1, 0), snap(2, 0, u"hi", 2, 2));
    ITextStoreACP* acp = store;
    // The editor's own change: told to the text services, outside any lock.
    store->Update(snap(2, 0, u"hi!", 3, 3));
    check(sink.text_changes.size() == 1 && sink.text_changes[0].acpStart == 2 && sink.text_changes[0].acpOldEnd == 2 &&
              sink.text_changes[0].acpNewEnd == 3 && sink.selection_changes == 1 && !sink.saw_lock_in_notification,
          "a character the editor took from a key: OnTextChange and OnSelectionChange, outside a lock");
    sink.clear();
    // A copy that arrives inside a lock waits for its end.
    lock(store, sink, TS_LF_READ | TS_LF_SYNC, [&](DWORD) {
        store->Update(snap(2, 0, u"hi!?", 4, 4));
        check(sink.text_changes.empty(), "not during a lock");
    });
    check(sink.text_changes.size() == 1 && sink.text_changes[0].acpStart == 3, "but when it ends");
    sink.clear();
    // An edit on its way: an older copy of the editor would undo it, and is dropped.
    lock(store, sink, TS_LF_READWRITE | TS_LF_SYNC, [&](DWORD) { acp->SetText(0, 4, 4, L"x", 1, nullptr); });
    store->Update(snap(2, 0, u"hi!?", 4, 4));
    check(sink.text_changes.empty() && text_of(acp) == u"<error>", "a copy older than the edit is dropped");
    lock(store, sink, TS_LF_READ | TS_LF_SYNC, [&](DWORD) { check(text_of(acp) == u"hi!?x", "the edit stays"); });
    store->Update(snap(2, 1, u"hi!?x", 5, 5));
    check(sink.text_changes.empty() && sink.selection_changes == 0, "the editor's answer to the edit changes nothing");
    // The app took less than was typed (a filter): the text services hear of it.
    lock(store, sink, TS_LF_READWRITE | TS_LF_SYNC, [&](DWORD) { acp->SetText(0, 5, 5, L"yz", 2, nullptr); });
    store->Update(snap(2, 2, u"hi!?xy", 6, 6));
    check(sink.text_changes.size() == 1 && sink.text_changes[0].acpStart == 6 && sink.text_changes[0].acpOldEnd == 7 &&
              sink.text_changes[0].acpNewEnd == 6,
          "a filtered edit comes back as a deletion");
    store->Update(snap(3, 0, u"other", 0, 0));
    check(sink.text_changes.size() == 1, "another editor's copy is not this document's");
    acp->Release();
}

void geometry() {
    Owner owner;
    Sink sink;
    tsf::Store* store = make(owner, sink, textinput::field_policy(1, 0), snap(5, 0, u"abשל", 4, 4));
    ITextStoreACP* acp = store;
    textinput::Layout l;
    l.epoch = 5;
    l.has_editor = true;
    l.editor = Box{0, 0, 200, 40};
    l.has_caret = true;
    l.caret = Box{40, 5, 41, 25};
    l.first = 0;
    l.boxes = {Box{10, 5, 20, 25}, Box{20, 5, 30, 25}, Box{50, 5, 60, 25}, Box{40, 5, 50, 25}};
    l.rtl = {false, false, true, true};
    store->Relayout(l);
    check(sink.layout_changes == 1, "a new layout: OnLayoutChange");
    store->Relayout(l);
    check(sink.layout_changes == 1, "the same layout again: nothing");
    lock(store, sink, TS_LF_READ | TS_LF_SYNC, [&](DWORD) {
        RECT r{};
        BOOL clipped = TRUE;
        acp->GetTextExt(tsf::Store::kView, 0, 2, &r, &clipped);
        check(r.left == 1010 && r.right == 1030 && r.top == 505 && r.bottom == 525 && !clipped,
              "GetTextExt: the characters' boxes, on the screen");
        acp->GetTextExt(tsf::Store::kView, 2, 4, &r, &clipped);
        check(r.left == 1040 && r.right == 1060, "a Hebrew word's box");
        acp->GetTextExt(tsf::Store::kView, 2, 2, &r, &clipped);
        check(r.left == 1060 && r.right == 1061, "the caret before a Hebrew letter: its right edge, a sliver");
        LONG at = -1;
        POINT p{1058, 510};
        acp->GetACPFromPoint(tsf::Store::kView, &p, GXFPF_ROUND_NEAREST, &at);
        check(at == 2, "a point on a Hebrew letter's right half is its start");
        p = POINT{1500, 900};
        check(acp->GetACPFromPoint(tsf::Store::kView, &p, 0, &at) == TS_E_INVALIDPOINT, "a point on nothing");
        RECT screen{};
        acp->GetScreenExt(tsf::Store::kView, &screen);
        check(screen.left == 1000 && screen.right == 1200, "GetScreenExt: the editor");
        HWND w = reinterpret_cast<HWND>(1);
        check(acp->GetWnd(tsf::Store::kView, &w) == S_OK && w == nullptr, "GetWnd: the owner's window");
    });
    // A composition grows past what the editor measured: no layout until it answers.
    lock(store, sink, TS_LF_READWRITE | TS_LF_SYNC, [&](DWORD) { acp->SetText(0, 4, 4, L"ab", 2, nullptr); });
    lock(store, sink, TS_LF_READ | TS_LF_SYNC, [&](DWORD) {
        RECT r{};
        BOOL clipped = FALSE;
        check(acp->GetTextExt(tsf::Store::kView, 5, 6, &r, &clipped) == TS_E_NOLAYOUT,
              "characters on their way to the editor: TS_E_NOLAYOUT");
    });
    store->Update(snap(5, 1, u"abשלab", 6, 6));
    l.acked = 1;
    l.boxes.push_back(Box{60, 5, 70, 25});
    l.boxes.push_back(Box{70, 5, 80, 25});
    l.rtl.push_back(false);
    l.rtl.push_back(false);
    sink.clear();
    store->Relayout(l);
    check(sink.layout_changes == 1, "the editor measured them: OnLayoutChange, to ask again");
    lock(store, sink, TS_LF_READ | TS_LF_SYNC, [&](DWORD) {
        RECT r{};
        BOOL clipped = FALSE;
        check(acp->GetTextExt(tsf::Store::kView, 5, 6, &r, &clipped) == S_OK && r.left == 1070, "and now they are there");
    });
    sink.clear();
    store->Moved();
    check(sink.layout_changes == 1, "the window moved: OnLayoutChange");
    acp->Release();
}

void password() {
    Owner owner;
    Sink sink;
    const textinput::FieldPolicy p = textinput::field_policy(1 | 0x80, 0);  // TYPE_TEXT_VARIATION_PASSWORD
    tsf::Store* store = make(owner, sink, p, snap(6, 0, u"secret", 6, 6));
    ITextStoreACP* acp = store;
    ITfContextOwnerCompositionSink* comp = store;
    TS_STATUS status{};
    acp->GetStatus(&status);
    check((status.dwDynamicFlags & TS_SD_READONLY) && !(status.dwStaticFlags & TS_SS_TKBPREDICTIONENABLE),
          "a password: read-only, no suggestions");
    lock(store, sink, TS_LF_READWRITE | TS_LF_SYNC, [&](DWORD) {
        check(text_of(acp).empty(), "its text is not shown to the text services");
        check(acp->SetText(0, 0, 0, L"x", 1, nullptr) == TS_E_READONLY, "nor written by them");
        View view;
        BOOL ok = TRUE;
        comp->OnStartComposition(&view, &ok);
        check(ok == FALSE, "and no composition starts in it");
    });
    store->Update(snap(6, 0, u"secret!", 7, 7));
    check(sink.text_changes.empty(), "a password's text never reaches them");
    check(owner.edits.empty(), "nothing edited");
    acp->Release();
}

void attributes() {
    Owner owner;
    Sink sink;
    const textinput::FieldPolicy p = textinput::field_policy(1 | 0x10, 0);  // TYPE_TEXT_VARIATION_URI
    tsf::Store* store = make(owner, sink, p, snap(7, 0, u"é\U0001F600B", 0, 0));
    ITextStoreACP* acp = store;
    const TS_ATTRID wanted[] = {tsf::kGUID_PROP_INPUTSCOPE, GUID_PROP_ATTRIBUTE};
    acp->RequestSupportedAttrs(TS_ATTR_FIND_WANT_VALUE, 2, wanted);
    TS_ATTRVAL values[2] = {};
    ULONG got = 0;
    acp->RetrieveRequestedAttrs(2, values, &got);
    bool url = false;
    if (got == 1 && values[0].varValue.vt == VT_UNKNOWN && values[0].varValue.punkVal) {
        ITfInputScope* scope = nullptr;
        values[0].varValue.punkVal->QueryInterface(__uuidof(ITfInputScope), reinterpret_cast<void**>(&scope));
        InputScope* list = nullptr;
        UINT n = 0;
        if (scope && SUCCEEDED(scope->GetInputScopes(&list, &n)) && n >= 1) url = list[0] == IS_URL;
        if (list) CoTaskMemFree(list);
        if (scope) scope->Release();
    }
    for (ULONG i = 0; i < got; ++i) VariantClear(&values[i].varValue);
    check(got == 1 && url, "GUID_PROP_INPUTSCOPE: an ITfInputScope with IS_URL for a URL field");
    acp->RequestSupportedAttrs(0, 1, &wanted[1]);
    acp->RetrieveRequestedAttrs(2, values, &got);
    check(got == 0, "an attribute the field has not: none");
    lock(store, sink, TS_LF_READ | TS_LF_SYNC, [&](DWORD) {
        IDataObject* data = nullptr;
        acp->GetFormattedText(0, -1, &data);
        FORMATETC f{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM m{};
        bool same = false;
        if (data && SUCCEEDED(data->GetData(&f, &m))) {
            const auto* t = static_cast<const char16_t*>(GlobalLock(m.hGlobal));
            same = t && std::u16string(t) == u"é\U0001F600B";
            GlobalUnlock(m.hGlobal);
            ReleaseStgMedium(&m);
        }
        if (data) data->Release();
        check(same, "GetFormattedText: the text, an emoji's two units with it");
    });
    acp->Release();
}

// ---- on the system's TSF ------------------------------------------------------------------------

bool under_wine() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version");
}

void system_session() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        check(false, "COM on the test's thread");
        return;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WinApkTsfTest";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"tsf", WS_OVERLAPPEDWINDOW, 0, 0, 300, 200, nullptr, nullptr,
                             wc.hInstance, nullptr);
    std::vector<textinput::Edit> edits;
    tsf::SessionHost host;
    host.edit = [&](const textinput::Edit& e) { edits.push_back(e); };
    host.to_screen = [](const Box&, RECT*) { return false; };
    host.from_screen = [](POINT, float*, float*) { return false; };
    HRESULT why = S_OK;
    auto session = tsf::Session::Create(w, host, &why);
    if (!session) {
        std::printf("note  no TSF thread manager here (0x%08lx)%s\n", static_cast<unsigned long>(why),
                    under_wine() ? ", as under Wine" : "");
        check(under_wine(), "Windows has TSF");
    } else {
        check(true, "the session activates the system's TSF");
        const bool focused = session->Focus(textinput::field_policy(1, 0), snap(9, 0, u"abc", 3, 3));
        check(focused == session->focused() && (focused || under_wine()), "a document for the focused editor");
        session->Update(snap(9, 0, u"abcd", 4, 4));
        MSG m{};
        m.hwnd = w;
        m.message = WM_KEYDOWN;
        m.wParam = 'A';
        m.lParam = 1;
        check(!session->Key(m), "a plain key with no input method composing is the app's");
        session->Blur();
        check(!session->focused(), "the editor goes, its document with it");
        check(session->Focus(textinput::field_policy(1 | 0x80, 0), snap(10, 0, u"pw", 2, 2)) == session->focused(),
              "a password's document, its keyboard disabled for the text services");
        check(!session->Focus(textinput::field_policy(0, 0), snap(11, 0, u"", 0, 0)) && !session->focused(),
              "TYPE_NULL: no document");
        session.reset();
        check(edits.empty(), "nothing was edited by a text service");
    }
    DestroyWindow(w);
    CoUninitialize();
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    interfaces();
    locks();
    edits();
    compositions();
    editor_changes();
    geometry();
    password();
    attributes();
    system_session();
    std::printf("%d TSF check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
