// Adapted from the other runtime (https://example.invalid/other-runtime), compat/input/text_store.cc,
// Copyright (C) 2026 its authors, SPDX-License-Identifier: Apache-2.0. Modified for Win.apk.
//
// tsf_text_store.cpp -- the focused editor as a TSF ACP document (tsf_text_store.h).
// Win.apk's changes: the namespace; GetTextExt answers with the caret for a range whose
// characters the editor did not lay out for us (a Compose field, a long text), so that an input
// method's candidate window still opens by the text, not in a corner of the screen.

#include "tsf_text_store.h"
#include <algorithm>
#include <climits>
#include <utility>
#include <new>
#include <cstring>

namespace winapk::host::tsf {
TextStore::TextStore(HWND window, Emit emit, Schedule schedule)
    : window_(window), emit_(std::move(emit)), schedule_(std::move(schedule)) {}
TextStore::~TextStore() { if (sink_ != nullptr) sink_->Release(); }
void TextStore::Disconnect() {
  // A manager retaining a closing COM object must never call its former
  // window/Java owner or retain that editor's text.
  emit_ = [](DocumentEdit) {};
  schedule_ = [] {};
  document_ = TextDocument{}; pending_document_.reset();
  geometry_ = TextGeometry{};
  caret_ = RECT{};
  pending_lock_ = 0; pending_layout_ = false; composing_ = false;
  window_ = nullptr;
}
HRESULT TextStore::QueryInterface(REFIID iid, void** out) {
  if (out == nullptr) return E_POINTER;
  *out = nullptr;
  if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_ITextStoreACP)) {
    *out = static_cast<ITextStoreACP*>(this);
  } else if (IsEqualIID(iid, IID_ITfContextOwnerCompositionSink)) {
    *out = static_cast<ITfContextOwnerCompositionSink*>(this);
  } else return E_NOINTERFACE;
  AddRef(); return S_OK;
}
ULONG TextStore::AddRef() { return InterlockedIncrement(&references_); }
ULONG TextStore::Release() {
  LONG count = InterlockedDecrement(&references_);
  if (count == 0) delete this;
  return static_cast<ULONG>(count);
}
namespace {
// The enumerator owns its cursor independently of the data object.
class TextFormatEnum final : public IEnumFORMATETC {
 public:
  explicit TextFormatEnum(ULONG cursor = 0) : cursor_(cursor) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IEnumFORMATETC)) return E_NOINTERFACE;
    *out = static_cast<IEnumFORMATETC*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override { LONG n = InterlockedDecrement(&refs_); if (!n) delete this; return n; }
  HRESULT STDMETHODCALLTYPE Next(ULONG count, FORMATETC* formats, ULONG* fetched) override {
    if (fetched) *fetched = 0;
    if (!formats || (count != 1 && !fetched)) return E_POINTER;
    ULONG n = 0;
    if (count && !cursor_) { formats[0] = {CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; cursor_ = 1; n = 1; }
    if (fetched) *fetched = n;
    return n == count ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
    ULONG available = 1 - cursor_; ULONG n = (std::min)(count, available); cursor_ += n;
    return n == count ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Reset() override { cursor_ = 0; return S_OK; }
  HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** out) override {
    if (!out) return E_POINTER;
    *out = new (std::nothrow) TextFormatEnum(cursor_);
    return *out ? S_OK : E_OUTOFMEMORY;
  }
 private: LONG refs_ = 1; ULONG cursor_ = 0;
};
class TextDataObject final : public IDataObject {
 public:
  TextDataObject(char16_t* text, size_t length) : text_(text), length_(length) {}
  ~TextDataObject() { delete[] text_; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IDataObject)) return E_NOINTERFACE;
    *out = static_cast<IDataObject*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override { LONG n = InterlockedDecrement(&refs_); if (!n) delete this; return n; }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override {
    if (!f) return E_POINTER;
    if (f->cfFormat != CF_UNICODETEXT || f->ptd) return DV_E_FORMATETC;
    if (f->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
    if (f->lindex != -1) return DV_E_LINDEX;
    if (!(f->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f, STGMEDIUM* medium) override {
    if (!medium) return E_POINTER;
    *medium = {};
    HRESULT hr = QueryGetData(f); if (FAILED(hr)) return hr;
    if (length_ > (SIZE_MAX / sizeof(char16_t)) - 1) return STG_E_MEDIUMFULL;
    SIZE_T bytes = (length_ + 1) * sizeof(char16_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes); if (!memory) return STG_E_MEDIUMFULL;
    void* data = GlobalLock(memory); if (!data) { GlobalFree(memory); return STG_E_MEDIUMFULL; }
    std::memcpy(data, text_, bytes); GlobalUnlock(memory);
    medium->tymed = TYMED_HGLOBAL; medium->hGlobal = memory; medium->pUnkForRelease = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
    if (!out) return E_POINTER;
    out->ptd = nullptr; return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (direction != DATADIR_GET) return direction == DATADIR_SET ? E_NOTIMPL : E_INVALIDARG;
    *out = new (std::nothrow) TextFormatEnum; return *out ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA** out) override {
    if (!out) return E_POINTER;
    *out = nullptr; return OLE_E_ADVISENOTSUPPORTED;
  }
 private: LONG refs_ = 1; char16_t* const text_; const size_t length_;
};
bool SameIdentity(IUnknown* left, IUnknown* right) {
  IUnknown* a = nullptr; IUnknown* b = nullptr;
  if (left != nullptr) left->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&a));
  if (right != nullptr) right->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&b));
  bool same = a != nullptr && a == b;
  if (a != nullptr) a->Release();
  if (b != nullptr) b->Release();
  return same;
}
}
HRESULT TextStore::AdviseSink(REFIID iid, IUnknown* unknown, DWORD mask) {
  if (unknown == nullptr) return E_POINTER;
  if (!IsEqualIID(iid, IID_ITextStoreACPSink)) return E_INVALIDARG;
  if (sink_ != nullptr) {
    if (!SameIdentity(sink_, unknown)) return CONNECT_E_ADVISELIMIT;
    sink_mask_ = mask; return S_OK;
  }
  HRESULT result = unknown->QueryInterface(iid, reinterpret_cast<void**>(&sink_));
  if (SUCCEEDED(result)) sink_mask_ = mask;
  return result;
}
HRESULT TextStore::UnadviseSink(IUnknown* unknown) {
  if (!SameIdentity(sink_, unknown)) return CONNECT_E_NOCONNECTION;
  sink_->Release(); sink_ = nullptr; sink_mask_ = 0; return S_OK;
}
bool TextStore::ReadLock() const { return (lock_ & TS_LF_READ) != 0; }
bool TextStore::WriteLock() const { return (lock_ & TS_LF_READWRITE) == TS_LF_READWRITE; }
bool TextStore::Range(LONG start, LONG end) const {
  return start >= 0 && end >= start && static_cast<size_t>(end) <= document_.text.size();
}
bool TextStore::FitsReplacement(LONG start, LONG end, ULONG count) const {
  size_t maximum = static_cast<size_t>(LONG_MAX - document_.offset);
  return Range(start, end) && count <= maximum
      && document_.text.size() - (end - start) <= maximum - count;
}
HRESULT TextStore::RequestLock(DWORD flags, HRESULT* session) {
  if (session == nullptr) return E_POINTER;
  if (sink_ == nullptr) return E_UNEXPECTED;
  if ((flags & TS_LF_READ) == 0 || (flags & ~(TS_LF_SYNC | TS_LF_READWRITE)) != 0) return E_INVALIDARG;
  if (lock_ != 0 || pending_document_.has_value()) {
    if ((flags & TS_LF_SYNC) != 0) { *session = TS_E_SYNCHRONOUS; return S_OK; }
    pending_lock_ |= flags & TS_LF_READWRITE;
    *session = TS_S_ASYNC; schedule_(); return S_OK;
  }
  lock_ = flags & TS_LF_READWRITE;
  *session = sink_->OnLockGranted(lock_);
  lock_ = 0;
  if (pending_lock_ != 0 || pending_document_.has_value()) schedule_();
  return S_OK;
}
void TextStore::Pump() {
  if (lock_ != 0 || pumping_) return;
  pumping_ = true;
  if (pending_document_.has_value()) {
    TextDocument next = std::move(*pending_document_); pending_document_.reset(); Apply(std::move(next));
  }
  if (pending_layout_) {
    pending_layout_ = false;
    if (sink_ != nullptr && (sink_mask_ & TS_AS_LAYOUT_CHANGE) != 0)
      sink_->OnLayoutChange(TS_LC_CHANGE, 0);
  }
  DWORD pending = pending_lock_; pending_lock_ = 0;
  if (pending != 0 && sink_ != nullptr) {
    HRESULT ignored; RequestLock(pending, &ignored);
  }
  pumping_ = false;
}
void TextStore::Update(TextDocument document) {
  if (!document.available) {
    document.text.clear(); document.start = document.end = document.offset = 0;
    document.reversed = false;
  }
  if (document.text.size() > LONG_MAX || document.start < 0 || document.end < document.start ||
      static_cast<size_t>(document.end) > document.text.size() || document.offset < 0 ||
      static_cast<size_t>(document.offset) > LONG_MAX - document.text.size()) return;
  if (document.epoch < document_.epoch || (pending_document_.has_value() &&
      document.epoch < pending_document_->epoch)) return;
  // Do not replace optimistic TSF edits with an older asynchronous editor reply.
  if (document.epoch == document_.epoch && document.applied_revision < revision_) return;
  if (lock_ != 0 || pumping_) { pending_document_ = std::move(document); schedule_(); return; }
  Apply(std::move(document));
}
void TextStore::Apply(TextDocument document) {
  bool changed = document.text != document_.text || document.epoch != document_.epoch;
  bool selection = changed || document.start != document_.start || document.end != document_.end
      || document.reversed != document_.reversed;
  bool status = document.available != document_.available;
  TS_TEXTCHANGE delta = {0, static_cast<LONG>(document_.text.size()), static_cast<LONG>(document.text.size())};
  if (document.epoch != document_.epoch) { revision_ = 0; composing_ = false; geometry_ = TextGeometry{}; }
  if (!document.available) { composing_ = false; geometry_ = TextGeometry{}; caret_ = RECT{}; }
  document_ = std::move(document);
  if (sink_ == nullptr) return;
  if (changed && (sink_mask_ & TS_AS_TEXT_CHANGE) != 0) sink_->OnTextChange(0, &delta);
  if (selection && (sink_mask_ & TS_AS_SEL_CHANGE) != 0) sink_->OnSelectionChange();
  if (status && (sink_mask_ & TS_AS_STATUS_CHANGE) != 0) sink_->OnStatusChange(TS_SD_READONLY);
}
void TextStore::Caret(const RECT& screen) {
  if (EqualRect(&caret_, &screen)) return;
  caret_ = screen;
  pending_layout_ = true; schedule_();
}
void TextStore::Geometry(TextGeometry geometry) {
  if (geometry.epoch != document_.epoch || geometry.start < 0 || geometry.text.size() > LONG_MAX
      || static_cast<size_t>(geometry.start) > LONG_MAX - geometry.text.size()) return;
  int32_t previous = -1;
  for (const auto& bounds : geometry.characters) {
    if (bounds.index <= previous || bounds.index < geometry.start
        || static_cast<size_t>(bounds.index - geometry.start) >= geometry.text.size()
        || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    previous = bounds.index;
  }
  if (geometry_.epoch == geometry.epoch && geometry_.start == geometry.start
      && geometry_.text == geometry.text && geometry_.characters == geometry.characters) return;
  geometry_ = std::move(geometry);
  pending_layout_ = true; schedule_();
}
bool TextStore::GeometryMatches() const {
  if (!document_.available || geometry_.epoch != document_.epoch
      || geometry_.start < document_.offset) return false;
  size_t first = static_cast<size_t>(geometry_.start - document_.offset);
  return first <= document_.text.size() && geometry_.text.size() <= document_.text.size() - first
      && document_.text.compare(first, geometry_.text.size(), geometry_.text) == 0;
}
void TextStore::Publish(DocumentEdit edit) {
  edit.epoch = document_.epoch; edit.revision = ++revision_;
  edit.start += document_.offset; edit.end += document_.offset;
  emit_(std::move(edit));
}
HRESULT TextStore::GetStatus(TS_STATUS* status) {
  if (status == nullptr) return E_POINTER;
  status->dwDynamicFlags = document_.available ? 0 : TS_SD_READONLY;
  status->dwStaticFlags = TS_SS_NOHIDDENTEXT; return S_OK;
}
HRESULT TextStore::QueryInsert(LONG start, LONG end, ULONG count, LONG* first, LONG* last) {
  if (!ReadLock()) return TS_E_NOLOCK;
  if (first == nullptr || last == nullptr) return E_POINTER;
  if (!FitsReplacement(start, end, count))
    return TS_E_INVALIDPOS;
  *first = start; *last = end; return S_OK;
}
HRESULT TextStore::GetSelection(ULONG index, ULONG count, TS_SELECTION_ACP* selection, ULONG* fetched) {
  if (!ReadLock()) return TS_E_NOLOCK;
  if (selection == nullptr || fetched == nullptr) return E_POINTER;
  *fetched = 0;
  if ((index != 0 && index != static_cast<ULONG>(TS_DEFAULT_SELECTION)) || count == 0) return E_INVALIDARG;
  *selection = TS_SELECTION_ACP{document_.start, document_.end,
      {document_.reversed ? TS_AE_START : TS_AE_END, FALSE}};
  *fetched = 1; return S_OK;
}
HRESULT TextStore::SetSelection(ULONG count, const TS_SELECTION_ACP* selection) {
  if (!WriteLock()) return TS_E_NOLOCK;
  if (selection == nullptr) return E_POINTER;
  if (!document_.available) return TS_E_READONLY;
  if (count != 1 || !Range(selection->acpStart, selection->acpEnd) ||
      selection->style.ase < TS_AE_NONE || selection->style.ase > TS_AE_END) return TS_E_INVALIDPOS;
  if (document_.start == selection->acpStart && document_.end == selection->acpEnd &&
      document_.reversed == (selection->style.ase == TS_AE_START)) return S_OK;
  if (revision_ == INT_MAX) return E_FAIL;
  document_.start = selection->acpStart; document_.end = selection->acpEnd;
  document_.reversed = selection->style.ase == TS_AE_START;
  DocumentEdit edit; edit.kind = DocumentEdit::kSelect;
  edit.start = document_.reversed ? document_.end : document_.start;
  edit.end = document_.reversed ? document_.start : document_.end;
  Publish(std::move(edit)); return S_OK;
}
HRESULT TextStore::GetText(LONG start, LONG end, WCHAR* text, ULONG requested, ULONG* copied,
                          TS_RUNINFO* runs, ULONG requested_runs, ULONG* returned_runs, LONG* next) {
  if (!ReadLock()) return TS_E_NOLOCK;
  if (copied == nullptr || returned_runs == nullptr || next == nullptr ||
      (requested != 0 && text == nullptr) || (requested_runs != 0 && runs == nullptr)) return E_POINTER;
  *copied = *returned_runs = 0;
  if (end == -1) end = static_cast<LONG>(document_.text.size());
  if (!Range(start, end)) return TS_E_INVALIDPOS;
  ULONG count = requested != 0 ? std::min(requested, static_cast<ULONG>(end - start))
                              : (requested_runs != 0 ? static_cast<ULONG>(end - start) : 0);
  if (requested != 0) {
    std::copy_n(document_.text.data() + start, count, text); *copied = count;
  }
  if (requested_runs != 0 && count != 0) { runs[0] = TS_RUNINFO{count, TS_RT_PLAIN}; *returned_runs = 1; }
  *next = start + count; return S_OK;
}
HRESULT TextStore::SetText(DWORD, LONG start, LONG end, const WCHAR* text, ULONG count, TS_TEXTCHANGE* change) {
  if (!WriteLock()) return TS_E_NOLOCK;
  if (change == nullptr || (count != 0 && text == nullptr)) return E_POINTER;
  if (!document_.available) return TS_E_READONLY;
  if (revision_ == INT_MAX) return E_FAIL;
  if (!FitsReplacement(start, end, count))
    return TS_E_INVALIDPOS;
  bool composition_text = composing_ && start >= composition_start_ && end <= composition_end_;
  LONG old_composition_start = composition_start_, old_composition_end = composition_end_;
  LONG new_composition_start = composition_start_, new_composition_end = composition_end_;
  if (composing_) {
    auto adjust = [&](LONG position, bool after) -> LONG {
      if (position < start) return position;
      if (position > end) return position - (end - start) + static_cast<LONG>(count);
      return start + (after ? static_cast<LONG>(count) : 0);
    };
    new_composition_start = adjust(composition_start_, false);
    new_composition_end = adjust(composition_end_, true);
  }
  bool select_after_composing = composition_text && start + static_cast<LONG>(count) != new_composition_end;
  if (composing_ && (!composition_text || select_after_composing) && revision_ >= INT_MAX - 1) return E_FAIL;
  composition_start_ = new_composition_start; composition_end_ = new_composition_end;
  std::u16string inserted;
  if (count != 0) inserted.assign(text, text + count);
  document_.text.replace(start, end - start, inserted);
  document_.start = document_.end = start + static_cast<LONG>(count);
  document_.reversed = false;
  *change = TS_TEXTCHANGE{start, end, document_.end};
  DocumentEdit edit;
  edit.kind = composition_text ? DocumentEdit::kCompositionText : DocumentEdit::kReplace;
  edit.start = composition_text ? old_composition_start : start;
  edit.end = composition_text ? old_composition_end : end;
  edit.text = composition_text ? document_.text.substr(composition_start_, composition_end_ - composition_start_)
                               : std::move(inserted);
  Publish(std::move(edit));
  if (select_after_composing) {
    DocumentEdit selection; selection.kind = DocumentEdit::kSelect;
    selection.start = selection.end = document_.end; Publish(std::move(selection));
  }
  if (composing_ && !composition_text) {
    DocumentEdit range; range.kind = DocumentEdit::kCompositionRange;
    range.start = composition_start_; range.end = composition_end_; Publish(std::move(range));
  }
  return S_OK;
}
HRESULT TextStore::InsertTextAtSelection(DWORD flags, const WCHAR* text, ULONG count,
                                       LONG* start, LONG* end, TS_TEXTCHANGE* change) {
  if (!ReadLock()) return TS_E_NOLOCK;
  if ((flags & TS_IAS_NOQUERY) != 0 && (flags & TS_IAS_QUERYONLY) != 0) return E_INVALIDARG;
  if ((flags & ~(TS_IAS_NOQUERY | TS_IAS_QUERYONLY)) != 0) return E_INVALIDARG;
  if ((flags & TS_IAS_NOQUERY) == 0 && (start == nullptr || end == nullptr)) return E_POINTER;
  if (!FitsReplacement(document_.start, document_.end, count))
    return TS_E_INVALIDPOS;
  LONG cursor = document_.start + static_cast<LONG>(count);
  if (start != nullptr) *start = cursor;
  if (end != nullptr) *end = cursor;
  if ((flags & TS_IAS_QUERYONLY) != 0) return S_OK;
  return SetText(0, document_.start, document_.end, text, count, change);
}
HRESULT TextStore::GetFormattedText(LONG start, LONG end, IDataObject** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (!WriteLock()) return TS_E_NOLOCK;
  if (end == -1) end = static_cast<LONG>(document_.text.size());
  if (!Range(start, end)) return TS_E_INVALIDPOS;
  size_t length = static_cast<size_t>(end - start);
  // CF_UNICODETEXT ends at the first NUL; do not silently lose a suffix.
  if (std::find(document_.text.begin() + start, document_.text.begin() + end, u'\0') != document_.text.begin() + end) return E_FAIL;
  if (length > (SIZE_MAX / sizeof(char16_t)) - 1) return E_OUTOFMEMORY;
  auto* copy = new (std::nothrow) char16_t[length + 1];
  if (!copy) return E_OUTOFMEMORY;
  std::copy_n(document_.text.data() + start, length, copy);
  copy[length] = 0;
  auto* object = new (std::nothrow) TextDataObject(copy, length);
  if (!object) { delete[] copy; return E_OUTOFMEMORY; }
  *out = object; return S_OK;
}
HRESULT TextStore::GetEmbedded(LONG, REFGUID, REFIID, IUnknown** out) { if (out) *out = nullptr; return E_NOTIMPL; }
HRESULT TextStore::QueryInsertEmbedded(const GUID*, const FORMATETC*, BOOL* okay) { if (!okay) return E_POINTER; *okay = FALSE; return S_OK; }
HRESULT TextStore::InsertEmbedded(DWORD, LONG, LONG, IDataObject*, TS_TEXTCHANGE*) { return E_NOTIMPL; }
HRESULT TextStore::InsertEmbeddedAtSelection(DWORD, IDataObject*, LONG*, LONG*, TS_TEXTCHANGE*) { return E_NOTIMPL; }
HRESULT TextStore::RequestSupportedAttrs(DWORD, ULONG, const TS_ATTRID*) { return S_OK; }
HRESULT TextStore::RequestAttrsAtPosition(LONG position, ULONG, const TS_ATTRID*, DWORD) { return Range(position, position) ? S_OK : TS_E_INVALIDPOS; }
HRESULT TextStore::RequestAttrsTransitioningAtPosition(LONG position, ULONG, const TS_ATTRID*, DWORD) { return Range(position, position) ? S_OK : TS_E_INVALIDPOS; }
HRESULT TextStore::FindNextAttrTransition(LONG start, LONG halt, ULONG, const TS_ATTRID*, DWORD,
                                        LONG* next, BOOL* found, LONG* offset) {
  if (!next || !found || !offset) return E_POINTER;
  if (!Range(start, halt)) return TS_E_INVALIDPOS;
  *next = halt; *found = FALSE; *offset = 0; return S_OK;
}
HRESULT TextStore::RetrieveRequestedAttrs(ULONG, TS_ATTRVAL*, ULONG* fetched) { if (!fetched) return E_POINTER; *fetched = 0; return S_OK; }
HRESULT TextStore::GetEndACP(LONG* end) { if (!ReadLock()) return TS_E_NOLOCK; if (!end) return E_POINTER; *end = static_cast<LONG>(document_.text.size()); return S_OK; }
HRESULT TextStore::GetActiveView(TsViewCookie* view) { if (!view) return E_POINTER; *view = 0; return S_OK; }
HRESULT TextStore::GetWnd(TsViewCookie view, HWND* window) { if (!window) return E_POINTER; if (view != 0) return E_INVALIDARG; *window = window_; return S_OK; }
HRESULT TextStore::GetScreenExt(TsViewCookie view, RECT* rectangle) {
  if (!rectangle) return E_POINTER;
  if (view != 0) return E_INVALIDARG;
  if (!GetClientRect(window_, rectangle)) return E_FAIL;
  MapWindowPoints(window_, nullptr, reinterpret_cast<POINT*>(rectangle), 2); return S_OK;
}
HRESULT TextStore::GetTextExt(TsViewCookie view, LONG start, LONG end, RECT* rectangle, BOOL* clipped) {
  if (!ReadLock()) return TS_E_NOLOCK;
  if (!rectangle || !clipped) return E_POINTER;
  if (view != 0 || !Range(start, end)) return TS_E_INVALIDPOS;
  if (!document_.available) return TS_E_NOLAYOUT;
  if (start == end && start == document_.end && caret_.bottom > caret_.top) {
    *rectangle = caret_; *clipped = FALSE; return S_OK;
  }
  if (!GeometryMatches()) {
    // No per-character layout for this text: the caret is the nearest place known.
    if (caret_.bottom <= caret_.top) return TS_E_NOLAYOUT;
    *rectangle = caret_; *clipped = FALSE; return S_OK;
  }
  if (start == end) return TS_E_NOLAYOUT;
  LONG first = start + document_.offset, last = end + document_.offset;
  auto character = std::lower_bound(geometry_.characters.begin(), geometry_.characters.end(), first,
      [](const TextCharacterBounds& bounds, LONG index) { return bounds.index < index; });
  RECT combined{}; bool cut = false;
  for (LONG index = first; index < last; ++index, ++character) {
    if (character == geometry_.characters.end() || character->index != index) return TS_E_NOLAYOUT;
    RECT bounds{character->left, character->top, character->right, character->bottom};
    if (index == first) combined = bounds; else UnionRect(&combined, &combined, &bounds);
    cut |= character->clipped;
  }
  RECT client{}, visible{};
  if (!GetClientRect(window_, &client) || !IntersectRect(&visible, &combined, &client)) return TS_E_NOLAYOUT;
  cut |= !EqualRect(&visible, &combined);
  MapWindowPoints(window_, nullptr, reinterpret_cast<POINT*>(&visible), 2);
  *rectangle = visible; *clipped = cut; return S_OK;
}
HRESULT TextStore::GetACPFromPoint(TsViewCookie view, const POINT* point, DWORD, LONG* position) {
  if (!ReadLock()) return TS_E_NOLOCK;
  if (!point || !position) return E_POINTER;
  if (view != 0) return E_INVALIDARG;
  if (!document_.available) return TS_E_NOLAYOUT;
  if (PtInRect(&caret_, *point)) { *position = document_.end; return S_OK; }
  if (!GeometryMatches()) return TS_E_NOLAYOUT;
  POINT client = *point; ScreenToClient(window_, &client);
  for (const auto& character : geometry_.characters) {
    RECT bounds{character.left, character.top, character.right, character.bottom};
    if (PtInRect(&bounds, client)) { *position = character.index - document_.offset; return S_OK; }
  }
  return TS_E_NOLAYOUT;
}
HRESULT TextStore::OnStartComposition(ITfCompositionView* composition, BOOL* okay) {
  if (!okay) return E_POINTER;
  *okay = document_.available;
  composition_start_ = document_.start; composition_end_ = document_.end;
  if (*okay && composition != nullptr) {
    ITfRange* range = nullptr; ITfRangeACP* acp = nullptr;
    LONG start = 0, length = 0;
    HRESULT result = composition->GetRange(&range);
    if (SUCCEEDED(result)) result = range->QueryInterface(IID_ITfRangeACP, reinterpret_cast<void**>(&acp));
    if (SUCCEEDED(result)) result = acp->GetExtent(&start, &length);
    bool valid = SUCCEEDED(result) && start >= 0 && length >= 0
        && static_cast<size_t>(start) <= document_.text.size()
        && static_cast<size_t>(length) <= document_.text.size() - start;
    if (valid) { composition_start_ = start; composition_end_ = start + length; }
    else *okay = FALSE;
    if (acp != nullptr) acp->Release();
    if (range != nullptr) range->Release();
  }
  composing_ = *okay != FALSE; return S_OK;
}
HRESULT TextStore::OnUpdateComposition(ITfCompositionView* composition, ITfRange* range) {
  if (!composing_) return S_OK;
  ITfRange* owned = nullptr;
  if (range == nullptr && composition != nullptr && SUCCEEDED(composition->GetRange(&owned))) range = owned;
  ITfRangeACP* acp = nullptr;
  HRESULT result = range == nullptr ? E_INVALIDARG : range->QueryInterface(IID_ITfRangeACP, reinterpret_cast<void**>(&acp));
  if (SUCCEEDED(result)) {
    LONG start = 0, length = 0; result = acp->GetExtent(&start, &length);
    if (SUCCEEDED(result) && start >= 0 && length >= 0 &&
        static_cast<size_t>(start) <= document_.text.size() &&
        static_cast<size_t>(length) <= document_.text.size() - start && revision_ != INT_MAX) {
      DocumentEdit edit; edit.kind = DocumentEdit::kCompositionRange; edit.start = start; edit.end = start + length;
      composition_start_ = start; composition_end_ = start + length;
      Publish(std::move(edit));
    }
    acp->Release();
  }
  if (owned != nullptr) owned->Release();
  return result;
}
HRESULT TextStore::OnEndComposition(ITfCompositionView*) {
  if (composing_) {
    composing_ = false;
    if (revision_ == INT_MAX) return E_FAIL;
    DocumentEdit edit; edit.kind = DocumentEdit::kCompositionEnd;
    if (Range(composition_start_, composition_end_)) {
      edit.start = composition_start_; edit.end = composition_end_;
      edit.text = document_.text.substr(composition_start_, composition_end_ - composition_start_);
    }
    Publish(std::move(edit));
  }
  return S_OK;
}
}  // namespace winapk::host::tsf
