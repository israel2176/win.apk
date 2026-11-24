// Adapted from the other runtime (https://example.invalid/other-runtime), probes/windows4/text_store_probe.cc,
// Copyright (C) 2026 its authors, SPDX-License-Identifier: Apache-2.0. Modified for Win.apk.
//
// tsf_test.cpp -- the TSF document over the focused editor (host/tsf_text_store.h) against
// ITextStoreACP's contract, with a sink standing in for TSF's manager: locks, reads and writes,
// the edits it hands the editor (epochs, revisions, composition, UTF-16 positions, Hebrew), the
// copies an editor sends after them, layout, and, at the end, the window-owned text service
// attached to a real TSF thread manager when the system has one (Windows; Wine's is partial:
// a failure to attach is reported, not failed, as the runtime then falls back to IMM32).
// Win.apk's changes: the namespace, the other runtime's test-service DLL left out, the caret fallback of
// GetTextExt, and the Hebrew and right-to-left checks.

#include <windows.h>
#include <cstdio>
#include <climits>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "tsf_text_service.h"
#include "tsf_text_store.h"

namespace {
using winapk::host::tsf::DocumentEdit;
using winapk::host::tsf::TextDocument;
using winapk::host::tsf::TextStore;
int failures = 0;
void Check(bool okay, const char* label) {
  std::printf("%s  tsf: %s\n", okay ? "PASS" : "FAIL", label);
  std::fflush(stdout);
  if (!okay) ++failures;
}
class Sink final : public ITextStoreACPSink {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_ITextStoreACPSink)) {
      *out = static_cast<ITextStoreACPSink*>(this); AddRef(); return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG count = InterlockedDecrement(&references_);
    if (count == 0) delete this;
    return static_cast<ULONG>(count);
  }
  HRESULT STDMETHODCALLTYPE OnTextChange(DWORD, const TS_TEXTCHANGE*) override { ++text; if (requesting) ++nested_notifications; return S_OK; }
  HRESULT STDMETHODCALLTYPE OnSelectionChange() override { ++selection; if (requesting) ++nested_notifications; return S_OK; }
  HRESULT STDMETHODCALLTYPE OnLayoutChange(TsLayoutCode, TsViewCookie) override { ++layout; if (requesting) ++nested_notifications; if (layout_action) layout_action(); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnStatusChange(DWORD) override { ++status; if (requesting) ++nested_notifications; return S_OK; }
  HRESULT STDMETHODCALLTYPE OnAttrsChange(LONG, LONG, ULONG, const TS_ATTRID*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnLockGranted(DWORD flags) override { ++locks; return action ? action(flags) : S_OK; }
  HRESULT STDMETHODCALLTYPE OnStartEditTransaction() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnEndEditTransaction() override { return S_OK; }
  HRESULT Lock(TextStore* store, DWORD flags = TS_LF_SYNC | TS_LF_READWRITE) {
    HRESULT session = E_FAIL;
    requesting = true;
    HRESULT result = store->RequestLock(flags, &session);
    requesting = false;
    return FAILED(result) ? result : session;
  }
  void Clear() { text = selection = layout = status = nested_notifications = 0; }
  std::function<HRESULT(DWORD)> action;
  std::function<void()> layout_action;
  int text = 0, selection = 0, layout = 0, status = 0, locks = 0, nested_notifications = 0;
  bool requesting = false;
 private:
  LONG references_ = 1;
};
std::u16string Text(TextStore* store, Sink* sink) {
  std::u16string result;
  sink->action = [&](DWORD) -> HRESULT {
    WCHAR buffer[256]; ULONG count = 0, runs = 0; LONG next = 0; TS_RUNINFO run;
    HRESULT status = store->GetText(0, -1, buffer, 256, &count, &run, 1, &runs, &next);
    if (SUCCEEDED(status)) result.assign(buffer, buffer + count);
    return status;
  };
  if (FAILED(sink->Lock(store, TS_LF_SYNC | TS_LF_READ))) ++failures;
  sink->action = nullptr;
  return result;
}
}


int main() {
  HWND window = CreateWindowExW(0, L"STATIC", L"Win.apk TSF test", WS_OVERLAPPED,
                               0, 0, 400, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!window) return 1;
  std::vector<DocumentEdit> edits;
  std::function<void(const DocumentEdit&)> host_feedback;
  int scheduled = 0;
  auto* store = new TextStore(window, [&](DocumentEdit edit) { edits.push_back(edit); if (host_feedback) host_feedback(edit); }, [&] { ++scheduled; });
  auto* sink = new Sink();
  Check(SUCCEEDED(store->AdviseSink(IID_ITextStoreACPSink, sink, TS_AS_ALL_SINKS)), "real ITextStoreACPSink subscription");
  void* identity = nullptr; void* composition = nullptr; void* identity2 = nullptr;
  store->QueryInterface(IID_IUnknown, &identity);
  store->QueryInterface(IID_ITfContextOwnerCompositionSink, &composition);
  static_cast<ITfContextOwnerCompositionSink*>(composition)->QueryInterface(IID_IUnknown, &identity2);
  Check(identity == identity2, "ACP and composition interfaces share canonical IUnknown identity");
  static_cast<IUnknown*>(identity)->Release(); static_cast<IUnknown*>(identity2)->Release();
  static_cast<ITfContextOwnerCompositionSink*>(composition)->Release();

  TextDocument original; original.available = true; original.epoch = 1;
  original.text = u"before \u05e9\u05dc\u05d5\u05dd"; original.start = 7; original.end = 11;
  store->Update(original);
  // Independent immutable object and medium ownership, exercised through the real ACP lock callback.
  TextDocument formatted; formatted.available = true; formatted.epoch = 1;
  formatted.text = u"A\u00e9\U0001f600B";
  store->Update(formatted);
  IDataObject* snapshot = nullptr;
  TextDocument with_nul = formatted; with_nul.text = std::u16string(u"A\0B", 3);
  store->Update(with_nul);
  sink->action = [&](DWORD) -> HRESULT {
    IDataObject* rejected = reinterpret_cast<IDataObject*>(1);
    Check(store->GetFormattedText(0, 3, &rejected) == E_FAIL && !rejected, "embedded NUL range rejected");
    IDataObject* suffix = nullptr;
    Check(store->GetFormattedText(2, 3, &suffix) == S_OK && suffix, "range excluding NUL accepted");
    if (suffix) suffix->Release();
    return S_OK;
  };
  Check(sink->Lock(store) == S_OK, "NUL policy checked under lock");
  sink->action = nullptr;
  Check(Text(store, sink) == with_nul.text, "NUL rejection preserves document");
  store->Update(formatted);
  Check(store->GetFormattedText(0, 1, &snapshot) == TS_E_NOLOCK && !snapshot, "formatted text requires lock");
  sink->action = [&](DWORD) -> HRESULT {
    Check(store->GetFormattedText(0, 1, &snapshot) == TS_E_NOLOCK && !snapshot, "read lock is insufficient for formatted text");
    return S_OK;
  };
  sink->Lock(store, TS_LF_SYNC | TS_LF_READ);
  sink->action = [&](DWORD) -> HRESULT {
    IDataObject* one = nullptr; IDataObject* empty = nullptr;
    Check(store->GetFormattedText(0, 1, &one) == S_OK && one, "end one selects exactly one unit");
    Check(store->GetFormattedText(2, 2, &empty) == S_OK && empty, "empty range accepted");
    FORMATETC f = {CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (one && empty) {
      STGMEDIUM m1 = {}, m2 = {};
      bool ok1 = one->GetData(&f, &m1) == S_OK;
      bool ok2 = empty->GetData(&f, &m2) == S_OK;
      const char16_t* t1 = ok1 ? static_cast<const char16_t*>(GlobalLock(m1.hGlobal)) : nullptr;
      const char16_t* t2 = ok2 ? static_cast<const char16_t*>(GlobalLock(m2.hGlobal)) : nullptr;
      Check(t1 && t2 && t1[0] == u'A' && t1[1] == 0 && t2[0] == 0, "one unit and empty exact terminators");
      if (t1) GlobalUnlock(m1.hGlobal);
      if (t2) GlobalUnlock(m2.hGlobal);
      if (ok1) ReleaseStgMedium(&m1);
      if (ok2) ReleaseStgMedium(&m2);
    }
    if (one) one->Release();
    if (empty) empty->Release();
    Check(store->GetFormattedText(3, 2, &snapshot) == TS_E_INVALIDPOS && !snapshot, "invalid range rejected");
    return store->GetFormattedText(1, -1, &snapshot);
  };
  Check(sink->Lock(store) == S_OK && snapshot, "partial snapshot created under write lock");
  sink->action = nullptr;
  if (snapshot) {
    FORMATETC format = {CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    Check(snapshot->QueryGetData(&format) == S_OK, "unicode text advertised");
    FORMATETC bad = format; bad.cfFormat = CF_TEXT;
    Check(snapshot->QueryGetData(&bad) == DV_E_FORMATETC, "unsupported format rejected");
    bad = format; bad.lindex = 0; Check(snapshot->QueryGetData(&bad) == DV_E_LINDEX, "index rejected");
    bad = format; bad.dwAspect = DVASPECT_ICON; Check(snapshot->QueryGetData(&bad) == DV_E_DVASPECT, "aspect rejected");
    bad = format; bad.tymed = TYMED_ISTREAM; Check(snapshot->QueryGetData(&bad) == DV_E_TYMED, "medium rejected");
    DVTARGETDEVICE device = {}; bad = format; bad.ptd = &device;
    Check(snapshot->QueryGetData(&bad) == DV_E_FORMATETC, "target device rejected");
    STGMEDIUM failed = {}; failed.tymed = TYMED_HGLOBAL; failed.hGlobal = reinterpret_cast<HGLOBAL>(1);
    Check(snapshot->GetData(&bad, &failed) == DV_E_FORMATETC && failed.tymed == TYMED_NULL && !failed.hGlobal && !failed.pUnkForRelease, "failed GetData clears medium");
    STGMEDIUM a = {}, b = {}; IEnumFORMATETC* enumeration = nullptr; IEnumFORMATETC* clone = nullptr;
    Check(snapshot->GetDataHere(&format, &a) == E_NOTIMPL, "GetDataHere unsupported");
    Check(snapshot->EnumFormatEtc(DATADIR_GET, &enumeration) == S_OK && enumeration, "format enumerator");
    if (enumeration) {
      FORMATETC found = {}; ULONG fetched = 0;
      Check(enumeration->Clone(&clone) == S_OK && clone, "independent enumerator clone");
      Check(enumeration->Next(1, &found, &fetched) == S_OK && fetched == 1 && found.cfFormat == CF_UNICODETEXT, "enumerates text");
      Check(enumeration->Next(1, &found, &fetched) == S_FALSE && fetched == 0, "enumeration ends");
      IEnumFORMATETC* at_end = nullptr;
      Check(enumeration->Clone(&at_end) == S_OK && at_end, "clone at end");
      if (at_end) { Check(at_end->Next(1, &found, &fetched) == S_FALSE && fetched == 0, "clone preserves cursor"); at_end->Release(); }
      Check(enumeration->Reset() == S_OK && enumeration->Skip(1) == S_OK && enumeration->Skip(1) == S_FALSE, "reset and skip cursor");
      Check(enumeration->Reset() == S_OK && enumeration->Skip(2) == S_FALSE && enumeration->Next(1, &found, &fetched) == S_FALSE, "partial skip reaches end");
      if (clone) { Check(clone->Next(1, &found, &fetched) == S_OK, "clone cursor independent"); clone->Release(); }
      enumeration->Release();
    }
    Check(snapshot->GetData(&format, &a) == S_OK, "first consumer medium");
    store->Disconnect();
    Check(store->Release() == 0, "source store destroyed");
    store = nullptr;
    Check(snapshot->GetData(&format, &b) == S_OK && a.hGlobal != b.hGlobal, "independent medium after source teardown");
    snapshot->AddRef(); snapshot->Release(); snapshot->Release(); snapshot = nullptr;
    const char16_t expected[] = u"\u00e9\U0001f600B";
    if (a.hGlobal && b.hGlobal) {
      const void* first = GlobalLock(a.hGlobal); const void* second = GlobalLock(b.hGlobal);
      Check(first && second && GlobalSize(a.hGlobal) >= sizeof(expected) && GlobalSize(b.hGlobal) >= sizeof(expected)
          && std::memcmp(first, expected, sizeof(expected)) == 0 && std::memcmp(second, expected, sizeof(expected)) == 0,
          "exact UTF-16 and terminator survive source teardown");
      if (first) GlobalUnlock(a.hGlobal);
      if (second) GlobalUnlock(b.hGlobal);
    }
    if (a.tymed != TYMED_NULL) ReleaseStgMedium(&a);
    if (b.tymed != TYMED_NULL) ReleaseStgMedium(&b);
  }
  // Restore a fresh store after testing teardown; keep the rest of the existing fixture intact.
  if (store) store->Release();
  store = new TextStore(window, [&](DocumentEdit edit) { edits.push_back(edit); if (host_feedback) host_feedback(edit); }, [&] { ++scheduled; });
  Check(SUCCEEDED(store->AdviseSink(IID_ITextStoreACPSink, sink, TS_AS_ALL_SINKS)), "subscribe after snapshot teardown");
  store->Update(original);
  LONG end = 0;
  Check(store->GetEndACP(&end) == TS_E_NOLOCK && Text(store, sink) == original.text,
        "UTF-16 document access requires a granted read lock");
  sink->action = [&](DWORD) -> HRESULT {
    TS_SELECTION_ACP selection; ULONG fetched = 0;
    HRESULT result = store->GetSelection(TS_DEFAULT_SELECTION, 1, &selection, &fetched);
    Check(SUCCEEDED(result) && fetched == 1 && selection.acpStart == 7 && selection.acpEnd == 11,
          "selection uses UTF-16 ACP positions");
    WCHAR buffer[2]; ULONG copied = 0, returned = 0; LONG next = 0; TS_RUNINFO run;
    result = store->GetText(7, -1, buffer, 2, &copied, &run, 1, &returned, &next);
    Check(SUCCEEDED(result) && copied == 2 && returned == 1 && run.uCount == 2 && next == 9,
          "bounded GetText returns a matching run and next ACP");
    TS_TEXTCHANGE change;
    Check(store->SetText(0, 7, 11, L"x", 1, &change) == TS_E_NOLOCK,
          "a read lock cannot mutate the document");
    HRESULT upgrade = E_FAIL;
    result = store->RequestLock(TS_LF_SYNC | TS_LF_READWRITE, &upgrade);
    Check(SUCCEEDED(result) && upgrade == TS_E_SYNCHRONOUS, "nested synchronous write locks are refused");
    return S_OK;
  };
  sink->Lock(store, TS_LF_SYNC | TS_LF_READ); sink->action = nullptr;
  sink->Clear();
  sink->action = [&](DWORD flags) -> HRESULT {
    if ((flags & TS_LF_READWRITE) == TS_LF_READWRITE) {
      TS_TEXTCHANGE change;
      HRESULT result = store->SetText(0, 7, 11, L"\u65e5\u672c", 2, &change);
      Check(SUCCEEDED(result) && change.acpStart == 7 && change.acpOldEnd == 11 && change.acpNewEnd == 9,
            "a real write grant replaces the selected document range");
      return result;
    }
    HRESULT session = E_FAIL;
    HRESULT result = store->RequestLock(TS_LF_READWRITE, &session);
    Check(SUCCEEDED(result) && session == TS_S_ASYNC, "read-to-write upgrade is asynchronous");
    return S_OK;
  };
  sink->Lock(store, TS_LF_SYNC | TS_LF_READ);
  Check(edits.empty() && scheduled > 0, "the upgrade does not run inside the original RequestLock");
  store->Pump(); sink->action = nullptr;
  Check(edits.size() == 1 && edits[0].text == u"\u65e5\u672c" && edits[0].start == 7 && edits[0].end == 11
          && edits[0].epoch == 1 && edits[0].revision == 1 && sink->text == 0 && sink->selection == 0,
        "TSF edits carry exact replacement/revision and do not echo change notifications");
  Check(Text(store, sink) == u"before \u65e5\u672c", "subsequent TSF reads observe the accepted edit");
  store->Update(original);
  Check(Text(store, sink) == u"before \u65e5\u672c", "an older asynchronous InputConnection reply cannot erase TSF edits");

  TextDocument accepted = original; accepted.text = u"before \u65e5\u672c"; accepted.start = accepted.end = 9; accepted.applied_revision = 1;
  store->Update(accepted);
  sink->Clear();
  sink->action = [&](DWORD) -> HRESULT {
    TextDocument next = accepted; next.text = u"external"; next.start = next.end = 8;
    store->Update(next);
    RECT caret = {10, 20, 11, 42}; store->Caret(caret);
    return S_OK;
  };
  sink->Lock(store, TS_LF_SYNC | TS_LF_READ); sink->action = nullptr;
  Check(sink->text == 0 && sink->layout == 0, "application document/layout changes are deferred outside RequestLock");
  store->Pump();
  Check(sink->text == 1 && sink->selection == 1 && sink->layout == 1 && sink->nested_notifications == 0,
        "deferred application text, selection and layout notifications are delivered once");
  sink->Clear();
  const size_t before_feedback = edits.size();
  RECT stable_caret = {20, 20, 21, 42};
  host_feedback = [&](const DocumentEdit&) { store->Caret(stable_caret); };
  sink->action = [&](DWORD) -> HRESULT {
    TS_SELECTION_ACP current; ULONG fetched = 0;
    HRESULT result = store->GetSelection(TS_DEFAULT_SELECTION, 1, &current, &fetched);
    return SUCCEEDED(result) && fetched == 1 ? store->SetSelection(1, &current) : E_FAIL;
  };
  sink->layout_action = [&] { sink->Lock(store); };
  store->Caret(stable_caret);
  for (int attempt = 0; attempt < 5; ++attempt) store->Pump();
  store->Caret(stable_caret); store->Pump();
  Check(sink->layout == 1 && edits.size() == before_feedback,
        "layout/identical-selection/host-caret COM feedback settles after one notification with no redundant edit");
  sink->layout_action = nullptr; host_feedback = nullptr; sink->action = nullptr;
  sink->action = [&](DWORD) -> HRESULT {
    TS_SELECTION_ACP reversed = {0, 4, {TS_AE_START, FALSE}};
    return store->SetSelection(1, &reversed);
  };
  sink->Lock(store); sink->Lock(store); sink->action = nullptr;
  Check(edits.size() == before_feedback + 1 && edits.back().kind == DocumentEdit::kSelect
          && edits.back().start == 4 && edits.back().end == 0,
        "a changed reverse selection emits once while an identical repeated selection is silent");
  TextDocument restricted; restricted.epoch = 2; restricted.text = u"must not leak"; restricted.available = false;
  store->Update(restricted);
  TS_STATUS status; store->GetStatus(&status);
  Check((status.dwDynamicFlags & TS_SD_READONLY) != 0 && Text(store, sink).empty(),
        "a restricted editor is read-only and retains no document text");
  store->Update(accepted);
  Check(Text(store, sink).empty(), "a reply from an old focused editor cannot expose its document");
  TextDocument composing; composing.available = true; composing.epoch = 3; composing.offset = 40;
  composing.text = u"A\U0001f600B"; composing.start = 1; composing.end = 3;
  store->Update(composing);
  size_t composition_edits = edits.size();
  sink->action = [&](DWORD) -> HRESULT {
    BOOL allowed = FALSE; TS_TEXTCHANGE change;
    HRESULT result = store->OnStartComposition(nullptr, &allowed);
    if (SUCCEEDED(result) && allowed) result = store->SetText(0, 1, 3, L"\u306b", 1, &change);
    LONG first = 0, last = 0;
    if (SUCCEEDED(result)) result = store->QueryInsert(1, 1, 4, &first, &last);
    if (SUCCEEDED(result)) result = store->SetText(0, 2, 2, L"\u307b\u3093", 2, &change);
    if (SUCCEEDED(result)) result = store->OnEndComposition(nullptr);
    return result;
  };
  HRESULT composed = sink->Lock(store); sink->action = nullptr;
  Check(SUCCEEDED(composed) && edits.size() == composition_edits + 3
          && edits[composition_edits].kind == DocumentEdit::kCompositionText
          && edits[composition_edits].start == 41 && edits[composition_edits].end == 43
          && edits[composition_edits].text == u"\u306b"
          && edits[composition_edits + 1].kind == DocumentEdit::kCompositionText
          && edits[composition_edits + 1].start == 41 && edits[composition_edits + 1].end == 42
          && edits[composition_edits + 1].text == u"\u306b\u307b\u3093"
          && edits.back().kind == DocumentEdit::kCompositionEnd
          && edits.back().text == u"\u306b\u307b\u3093",
        "TSF composition transactions preserve full composing text, UTF-16 replacement ranges, offsets and the native final commit");
  winapk::host::tsf::TextGeometry geometry; geometry.epoch = 3; geometry.start = 41;
  geometry.text = u"\u306b\u307b\u3093";
  geometry.characters = {{41, 20, 20, 30, 42, false}, {42, 30, 20, 40, 42, false},
      {43, 40, 20, 50, 42, false}};
  store->Geometry(geometry);
  sink->action = [&](DWORD) -> HRESULT {
    RECT rectangle{}; BOOL clipped = TRUE;
    POINT expected{20, 20}; ClientToScreen(window, &expected);
    HRESULT result = store->GetTextExt(0, 1, 4, &rectangle, &clipped);
    Check(SUCCEEDED(result) && !clipped && rectangle.left == expected.x && rectangle.top == expected.y
            && rectangle.right == expected.x + 30 && rectangle.bottom == expected.y + 22,
        "TSF composing-range extents use actual measured UTF-16 character bounds in screen coordinates");
    POINT second{35, 30}; ClientToScreen(window, &second); LONG acp = -1;
    Check(store->GetACPFromPoint(0, &second, 0, &acp) == S_OK && acp == 2,
        "measured character hit testing preserves ExtractedText offset and ACP indices");
    LONG first = 0, last = 0;
    Check(store->QueryInsert(0, 0, static_cast<ULONG>(LONG_MAX), &first, &last) == TS_E_INVALIDPOS,
        "replacement limits include the absolute extracted-text offset without integer overflow");
    return S_OK;
  };
  sink->Lock(store); sink->action = nullptr;
  auto clipped_geometry = geometry;
  clipped_geometry.characters.back().right = 500;
  store->Geometry(clipped_geometry);
  sink->action = [&](DWORD) -> HRESULT {
    RECT rectangle{}, client{}; BOOL clipped = FALSE;
    GetClientRect(window, &client); POINT edge{client.right, 0}; ClientToScreen(window, &edge);
    HRESULT result = store->GetTextExt(0, 1, 4, &rectangle, &clipped);
    Check(SUCCEEDED(result) && clipped && rectangle.right == edge.x,
        "measured TSF range extents report actual client clipping without invented off-window geometry");
    return S_OK;
  };
  sink->Lock(store); sink->action = nullptr;
  auto missing_geometry = geometry;
  missing_geometry.characters.pop_back(); store->Geometry(missing_geometry);
  sink->action = [&](DWORD) -> HRESULT {
    RECT rectangle{}; BOOL clipped = FALSE;
    Check(store->GetTextExt(0, 1, 4, &rectangle, &clipped) == TS_E_NOLAYOUT,
        "a range missing any measured UTF-16 character returns TS_E_NOLAYOUT");
    return S_OK;
  };
  sink->Lock(store); sink->action = nullptr;
  geometry.text = u"old"; store->Geometry(geometry);
  sink->action = [&](DWORD) -> HRESULT {
    RECT rectangle{}; BOOL clipped = FALSE;
    Check(store->GetTextExt(0, 1, 4, &rectangle, &clipped) == TS_E_NOLAYOUT,
        "stale measured text never supplies invented TSF range geometry");
    return S_OK;
  };
  sink->Lock(store); sink->action = nullptr;
  TextDocument partial; partial.available = true; partial.epoch = 4;
  partial.text = u"abcd"; partial.start = 0; partial.end = 4;
  store->Update(partial);
  size_t partial_edits = edits.size();
  sink->action = [&](DWORD) -> HRESULT {
    BOOL allowed = FALSE; TS_TEXTCHANGE change;
    HRESULT result = store->OnStartComposition(nullptr, &allowed);
    if (SUCCEEDED(result) && allowed) result = store->SetText(0, 1, 2, L"X", 1, &change);
    if (SUCCEEDED(result)) result = store->OnEndComposition(nullptr);
    return result;
  };
  HRESULT partial_result = sink->Lock(store); sink->action = nullptr;
  Check(SUCCEEDED(partial_result) && edits.size() == partial_edits + 3
          && edits[partial_edits].kind == DocumentEdit::kCompositionText
          && edits[partial_edits].text == u"aXcd"
          && edits[partial_edits + 1].kind == DocumentEdit::kSelect
          && edits[partial_edits + 1].start == 2 && edits[partial_edits + 1].end == 2
          && edits.back().kind == DocumentEdit::kCompositionEnd && edits.back().text == u"aXcd",
        "a partial edit inside a TSF composition restores the native insertion caret after publishing the full composing text");
  TextDocument live; live.available = true; live.epoch = 5; live.text = u"abc"; live.start = live.end = 3;
  store->Update(live);
  RECT live_caret{20, 20, 22, 42};
  MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&live_caret), 2);
  store->Caret(live_caret);
  TextDocument unavailable; unavailable.epoch = 5;
  store->Update(unavailable);
  sink->action = [&](DWORD) -> HRESULT {
    RECT rectangle{}; BOOL clipped = FALSE; LONG position = -1;
    POINT old_point{live_caret.left, live_caret.top};
    Check(store->GetTextExt(0, 0, 0, &rectangle, &clipped) == TS_E_NOLAYOUT
            && store->GetACPFromPoint(0, &old_point, 0, &position) == TS_E_NOLAYOUT,
        "an unavailable editor exposes neither the previous caret nor measured range geometry");
    return S_OK;
  };
  sink->Lock(store); sink->action = nullptr;
  live.epoch = 6; live.text = u"XYZ"; store->Update(live);
  geometry.epoch = 6; geometry.start = 0; geometry.text = u"XYZ";
  geometry.characters = {{0, 20, 20, 30, 42, false}, {1, 30, 20, 40, 42, false}, {2, 40, 20, 50, 42, false}};
  store->Geometry(geometry); store->Caret(live_caret);
  sink->action = [&](DWORD) -> HRESULT {
    RECT rectangle{}; BOOL clipped = TRUE;
    Check(store->GetTextExt(0, 0, 3, &rectangle, &clipped) == S_OK && !clipped
            && rectangle.left == live_caret.left && rectangle.right == live_caret.left + 30,
        "a new editor epoch recovers only from its own fresh text and geometry");
    return S_OK;
  };
  sink->Lock(store); sink->action = nullptr;
  ITextStoreACP* held = nullptr;
  HRESULT retained = store->QueryInterface(IID_ITextStoreACP, reinterpret_cast<void**>(&held));
  store->Disconnect();
  sink->action = [&](DWORD) -> HRESULT {
    RECT rectangle{}; BOOL clipped = FALSE; LONG position = -1;
    POINT old_point{live_caret.left, live_caret.top};
    Check(SUCCEEDED(retained) && held != nullptr
            && held->GetTextExt(0, 0, 0, &rectangle, &clipped) == TS_E_NOLAYOUT
            && held->GetACPFromPoint(0, &old_point, 0, &position) == TS_E_NOLAYOUT,
        "a retained COM interface cannot expose a retired editor's caret or text geometry after Disconnect");
    return S_OK;
  };
  sink->Lock(store); sink->action = nullptr;
  if (held != nullptr) held->Release();
  Check(SUCCEEDED(store->UnadviseSink(sink)), "the actual sink can unsubscribe by canonical COM identity");
  store->Release(); sink->Release();
  {
    // Win.apk: GetTextExt with no laid-out characters answers with the caret when it is known,
    // and Hebrew (right-to-left) text is plain UTF-16 to the document.
    std::vector<DocumentEdit> rtl_edits;
    auto* rtl = new TextStore(window, [&](DocumentEdit edit) { rtl_edits.push_back(edit); }, [] {});
    auto* rtl_sink = new Sink();
    rtl->AdviseSink(IID_ITextStoreACPSink, rtl_sink, TS_AS_ALL_SINKS);
    TextDocument hebrew; hebrew.available = true; hebrew.epoch = 1; hebrew.offset = 3;
    hebrew.text = u"שלום"; hebrew.start = hebrew.end = 4;
    rtl->Update(hebrew);
    rtl_sink->action = [&](DWORD) -> HRESULT {
      RECT r{}; BOOL clipped = TRUE;
      Check(rtl->GetTextExt(0, 0, 2, &r, &clipped) == TS_E_NOLAYOUT, "no caret and no layout: TS_E_NOLAYOUT");
      return S_OK;
    };
    rtl_sink->Lock(rtl);
    RECT caret{100, 50, 101, 70};
    rtl->Caret(caret); rtl->Pump();
    rtl_sink->action = [&](DWORD) -> HRESULT {
      RECT r{}; BOOL clipped = TRUE;
      HRESULT hr = rtl->GetTextExt(0, 0, 2, &r, &clipped);
      Check(hr == S_OK && EqualRect(&r, &caret) && !clipped, "a range with no laid-out characters is at the caret");
      TS_TEXTCHANGE change;
      BOOL allowed = FALSE;
      hr = rtl->OnStartComposition(nullptr, &allowed);
      if (SUCCEEDED(hr) && allowed) hr = rtl->SetText(0, 4, 4, L"א", 1, &change);
      if (SUCCEEDED(hr)) hr = rtl->OnEndComposition(nullptr);
      return hr;
    };
    HRESULT composed = rtl_sink->Lock(rtl);
    rtl_sink->action = nullptr;
    Check(SUCCEEDED(composed) && rtl_edits.size() == 2 && rtl_edits[0].kind == DocumentEdit::kCompositionText &&
              rtl_edits[0].start == 7 && rtl_edits[0].end == 7 && rtl_edits[0].text == u"א" &&
              rtl_edits[1].kind == DocumentEdit::kCompositionEnd && rtl_edits[1].text == u"א",
          "a Hebrew composition at the end of Hebrew text: the editor's positions, the offset added");
    Check(Text(rtl, rtl_sink) == u"שלוםא", "the document holds the composed Hebrew");
    rtl->Disconnect();
    rtl->UnadviseSink(rtl_sink);
    rtl->Release();
    rtl_sink->Release();
  }
  HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (SUCCEEDED(initialized)) {
    std::vector<DocumentEdit> attached_edits;
    TextDocument initial = original; initial.epoch = 20;
    HRESULT attached = E_FAIL;
    auto service = winapk::host::tsf::TextService::Attach(window, initial,
        [&](DocumentEdit edit) { attached_edits.push_back(std::move(edit)); }, [] {}, &attached);
    if (service == nullptr) {
      std::printf("SKIP  tsf: no TSF thread manager here (0x%08lx): the runtime falls back to IMM32\n",
                  static_cast<unsigned long>(attached));
    } else {
      service->Focus(true);
      TextDocument next = initial; next.text = u"שלום world"; next.start = next.end = 4;
      service->Update(next);
      service->Pump();
      service->Retire();
      service->Update(initial);  // a retired service takes nothing more
      service.reset();
      Check(attached_edits.empty(), "an attached document makes no edit of its own, and retires and detaches cleanly");
    }
    CoUninitialize();
  } else { Check(false, "the test's STA initializes for the window-owned text service"); }
  DestroyWindow(window);
  std::printf("%d TSF check(s) failed\n", failures);
  return failures;
}
