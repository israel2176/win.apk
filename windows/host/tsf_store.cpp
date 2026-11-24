// tsf_store.cpp -- see tsf_store.h.

#include "tsf_store.h"

#include <inputscope.h>
#include <objidl.h>
#include <olectl.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace winapk::host::tsf {

// {F86AD89F-5FE4-4B8D-BB9F-EF3797A84F1F}
const IID kIID_ITextStoreACP2 = {0xf86ad89f, 0x5fe4, 0x4b8d, {0xbb, 0x9f, 0xef, 0x37, 0x97, 0xa8, 0x4f, 0x1f}};
// {1713DD5A-68E7-4A5B-9AF6-592A595C778D}
const GUID kGUID_PROP_INPUTSCOPE = {0x1713dd5a, 0x68e7, 0x4a5b, {0x9a, 0xf6, 0x59, 0x2a, 0x59, 0x5c, 0x77, 0x8d}};

namespace {

using textinput::Box;

// text_input_model.h names Windows' input scopes by value; these are the header's.
static_assert(textinput::scope::kDefault == IS_DEFAULT && textinput::scope::kUrl == IS_URL &&
                  textinput::scope::kEmailAddress == IS_EMAIL_SMTPEMAILADDRESS &&
                  textinput::scope::kPersonName == IS_PERSONALNAME_FULLNAME &&
                  textinput::scope::kPostalAddress == IS_ADDRESS_FULLPOSTALADDRESS &&
                  textinput::scope::kDate == IS_DATE_FULLDATE && textinput::scope::kDigits == IS_DIGITS &&
                  textinput::scope::kNumber == IS_NUMBER && textinput::scope::kPassword == IS_PASSWORD &&
                  textinput::scope::kTelephone == IS_TELEPHONE_FULLTELEPHONENUMBER &&
                  textinput::scope::kTime == IS_TIME_FULLTIME &&
                  textinput::scope::kAlphanumericHalfwidth == IS_ALPHANUMERIC_HALFWIDTH &&
                  textinput::scope::kSearch == IS_SEARCH && textinput::scope::kSearchIncremental == IS_SEARCH_INCREMENTAL &&
                  textinput::scope::kYomi == IS_YOMI && textinput::scope::kChat == IS_CHAT &&
                  textinput::scope::kPrivate == IS_PRIVATE && textinput::scope::kNumericPassword == IS_NUMERIC_PASSWORD,
              "InputScope values");

// The field's kind for the text services that ask (GUID_PROP_INPUTSCOPE's value).
class ScopeList final : public ITfInputScope {
public:
    explicit ScopeList(std::vector<int32_t> scopes) : scopes_(std::move(scopes)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (riid == IID_IUnknown || riid == __uuidof(ITfInputScope)) *out = static_cast<ITfInputScope*>(this);
        if (!*out) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE GetInputScopes(InputScope** scopes, UINT* count) override {
        if (!scopes || !count) return E_INVALIDARG;
        const size_t n = std::max<size_t>(scopes_.size(), 1);
        auto* list = static_cast<InputScope*>(CoTaskMemAlloc(n * sizeof(InputScope)));
        if (!list) return E_OUTOFMEMORY;
        if (scopes_.empty()) list[0] = IS_DEFAULT;
        for (size_t i = 0; i < scopes_.size(); ++i) list[i] = static_cast<InputScope>(scopes_[i]);
        *scopes = list;
        *count = static_cast<UINT>(n);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPhrase(BSTR**, UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRegularExpression(BSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSRGS(BSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetXML(BSTR*) override { return E_NOTIMPL; }

private:
    ~ScopeList() = default;
    LONG refs_ = 1;
    std::vector<int32_t> scopes_;
};

// GetFormattedText's answer: the range as CF_UNICODETEXT, the one format a plain editor has.
class PlainText final : public IDataObject {
public:
    explicit PlainText(std::u16string text) : text_(std::move(text)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDataObject) *out = static_cast<IDataObject*>(this);
        if (!*out) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f, STGMEDIUM* m) override {
        if (!f || !m) return E_INVALIDARG;
        const HRESULT ok = QueryGetData(f);
        if (ok != S_OK) return ok;
        const size_t bytes = (text_.size() + 1) * sizeof(char16_t);
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!g) return E_OUTOFMEMORY;
        void* p = GlobalLock(g);
        std::memcpy(p, text_.c_str(), bytes);
        GlobalUnlock(g);
        m->tymed = TYMED_HGLOBAL;
        m->hGlobal = g;
        m->pUnkForRelease = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override {
        if (!f) return E_INVALIDARG;
        if (f->cfFormat != CF_UNICODETEXT) return DV_E_FORMATETC;
        if (!(f->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
        return f->dwAspect == DVASPECT_CONTENT ? S_OK : DV_E_DVASPECT;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out) out->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    ~PlainText() = default;
    LONG refs_ = 1;
    std::u16string text_;
};

// A range's place in ACP, through ITfRangeACP.
bool range_extent(ITfRange* range, LONG* start, LONG* end) {
    if (!range) return false;
    ITfRangeACP* acp = nullptr;
    if (FAILED(range->QueryInterface(IID_ITfRangeACP, reinterpret_cast<void**>(&acp))) || !acp) return false;
    LONG s = 0, n = 0;
    const HRESULT hr = acp->GetExtent(&s, &n);
    acp->Release();
    if (FAILED(hr)) return false;
    *start = s;
    *end = s + n;
    return true;
}

textinput::DisplayAttribute plain(const TF_DISPLAYATTRIBUTE& d) {
    textinput::DisplayAttribute a;
    a.text_type = d.crText.type;
    a.text = d.crText.type == TF_CT_SYSCOLOR ? static_cast<uint32_t>(d.crText.nIndex) : d.crText.cr;
    a.back_type = d.crBk.type;
    a.back = d.crBk.type == TF_CT_SYSCOLOR ? static_cast<uint32_t>(d.crBk.nIndex) : d.crBk.cr;
    a.line_style = d.lsStyle;
    a.bold_line = d.fBoldLine != FALSE;
    a.attr = d.bAttr;
    return a;
}

}  // namespace

Store::Store(StoreOwner* owner, textinput::FieldPolicy policy, const textinput::Snapshot& initial)
    : owner_(owner), policy_(std::move(policy)) {
    textinput::Snapshot s = initial;
    if (policy_.hide_text) {
        s.text.clear();
        s.offset = s.anchor = s.caret = 0;
    }
    doc_.reset(s);
    scopes_ = new ScopeList(policy_.scopes);
}

Store::~Store() {
    if (sink_) sink_->Release();
    if (composition_) composition_->Release();
    if (scopes_) scopes_->Release();
}

HRESULT Store::QueryInterface(REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITextStoreACP) {
        *out = static_cast<ITextStoreACP*>(this);
    } else if (riid == kIID_ITextStoreACP2) {
        *out = static_cast<TextStoreACP2*>(this);
    } else if (riid == IID_ITfContextOwnerCompositionSink) {
        *out = static_cast<ITfContextOwnerCompositionSink*>(this);
    } else if (riid == IID_ITfTextEditSink) {
        *out = static_cast<ITfTextEditSink*>(this);
    }
    if (!*out) return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

ULONG Store::AddRef() { return static_cast<ULONG>(++refs_); }

ULONG Store::Release() {
    const LONG n = --refs_;
    if (n == 0) delete this;
    return static_cast<ULONG>(n);
}

// ---- the owner's side ------------------------------------------------------------------------

void Store::Update(const textinput::Snapshot& snapshot) {
    if (!connected_) return;
    textinput::Snapshot s = snapshot;
    if (policy_.hide_text) {
        s.text.clear();
        s.offset = s.anchor = s.caret = 0;
    }
    pending_snapshot_ = std::move(s);
    Flush();
}

void Store::Relayout(textinput::Layout layout) {
    if (!connected_ || layout.epoch != doc_.epoch()) return;
    auto same = [](const Box& a, const Box& b) {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    };
    bool changed = layout.has_editor != layout_.has_editor || layout.has_caret != layout_.has_caret ||
                   layout.first != layout_.first || layout.boxes.size() != layout_.boxes.size() ||
                   !same(layout.editor, layout_.editor) || !same(layout.caret, layout_.caret) ||
                   layout.rtl != layout_.rtl;
    for (size_t i = 0; !changed && i < layout.boxes.size(); ++i) changed = !same(layout.boxes[i], layout_.boxes[i]);
    layout_ = std::move(layout);
    if (!changed) return;
    pending_layout_ = true;
    Flush();
}

void Store::Moved() {
    if (!connected_) return;
    pending_layout_ = true;
    Flush();
}

void Store::Disconnect() {
    connected_ = false;
    queue_.clear();
    pending_snapshot_.reset();
    if (sink_) {
        sink_->Release();
        sink_ = nullptr;
    }
    if (composition_) {
        composition_->Release();
        composition_ = nullptr;
    }
}

// ---- locks -----------------------------------------------------------------------------------

HRESULT Store::AdviseSink(REFIID riid, IUnknown* unknown, DWORD mask) {
    if (!connected_) return E_UNEXPECTED;
    if (!unknown) return E_INVALIDARG;
    if (riid != IID_ITextStoreACPSink) return E_INVALIDARG;
    ITextStoreACPSink* sink = nullptr;
    if (FAILED(unknown->QueryInterface(IID_ITextStoreACPSink, reinterpret_cast<void**>(&sink))) || !sink) {
        return E_NOINTERFACE;
    }
    if (sink_) {
        // The same sink again only changes its mask; a second sink is refused (one per store).
        IUnknown* a = nullptr;
        IUnknown* b = nullptr;
        sink_->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&a));
        sink->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&b));
        const bool same = a == b;
        if (a) a->Release();
        if (b) b->Release();
        sink->Release();
        if (!same) return CONNECT_E_ADVISELIMIT;
        sink_mask_ = mask;
        return S_OK;
    }
    sink_ = sink;
    sink_mask_ = mask;
    return S_OK;
}

HRESULT Store::UnadviseSink(IUnknown* unknown) {
    if (!sink_ || !unknown) return CONNECT_E_NOCONNECTION;
    IUnknown* a = nullptr;
    IUnknown* b = nullptr;
    sink_->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&a));
    unknown->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&b));
    const bool same = a && a == b;
    if (a) a->Release();
    if (b) b->Release();
    if (!same) return CONNECT_E_NOCONNECTION;
    sink_->Release();
    sink_ = nullptr;
    sink_mask_ = 0;
    return S_OK;
}

HRESULT Store::RequestLock(DWORD flags, HRESULT* session) {
    if (!session) return E_INVALIDARG;
    *session = E_FAIL;
    if (!sink_ || !connected_) return E_UNEXPECTED;
    const DWORD kind = flags & TS_LF_READWRITE;
    if (kind != TS_LF_READ && kind != TS_LF_READWRITE) return E_INVALIDARG;
    if (lock_ != 0) {
        // One lock at a time. A synchronous request cannot wait; an asynchronous one (also a
        // read lock's holder asking to write) is granted when this lock ends.
        if (flags & TS_LF_SYNC) {
            *session = TS_E_SYNCHRONOUS;
        } else {
            queue_.push_back(kind);
            *session = TS_S_ASYNC;
        }
        return S_OK;
    }
    *session = Grant(kind);
    while (!queue_.empty() && lock_ == 0 && sink_ && connected_) {
        const DWORD next = queue_.front();
        queue_.pop_front();
        Grant(next);
    }
    Flush();
    return S_OK;
}

HRESULT Store::Grant(DWORD kind) {
    AddRef();  // a text service may let go of the context inside its lock
    ITextStoreACPSink* sink = sink_;
    sink->AddRef();
    lock_ = kind;
    const HRESULT hr = sink->OnLockGranted(kind);
    if (kind == TS_LF_READWRITE) EndOfChange();
    lock_ = 0;
    sink->Release();
    Release();
    return hr;
}

void Store::EndOfChange() {
    if (!connected_) return;
    ReadComposition();
    if (auto e = doc_.take_edit()) owner_->edit(*e);
}

void Store::Flush() {
    if (lock_ != 0 || flushing_ || !connected_) return;
    flushing_ = true;
    AddRef();
    for (int round = 0; round < 8 && connected_; ++round) {
        if (pending_snapshot_) {
            textinput::Snapshot s = std::move(*pending_snapshot_);
            pending_snapshot_.reset();
            if (auto x = doc_.accept(s)) {
                if (x->text) {
                    if (pending_text_) {
                        // Two changes as one: from the first's start to the second's end.
                        textinput::TextChange& t = *pending_text_;
                        const int32_t old_end = std::max(t.old_end, x->text->old_end - (t.new_end - t.old_end));
                        t.new_end = std::max(t.new_end + (x->text->new_end - x->text->old_end),
                                             x->text->new_end);
                        t.start = std::min(t.start, x->text->start);
                        t.old_end = old_end;
                    } else {
                        pending_text_ = x->text;
                    }
                }
                if (x->selection) pending_selection_ = true;
            }
        }
        if (!sink_) {
            pending_text_.reset();
            pending_selection_ = pending_layout_ = false;
            break;
        }
        ITextStoreACPSink* sink = sink_;
        sink->AddRef();
        bool any = false;
        if (pending_text_) {
            TS_TEXTCHANGE c{pending_text_->start, pending_text_->old_end, pending_text_->new_end};
            pending_text_.reset();
            any = true;
            if (sink_mask_ & TS_AS_TEXT_CHANGE) sink->OnTextChange(0, &c);
        }
        if (pending_selection_) {
            pending_selection_ = false;
            any = true;
            if (sink_mask_ & TS_AS_SEL_CHANGE) sink->OnSelectionChange();
        }
        if (pending_layout_) {
            pending_layout_ = false;
            any = true;
            if (sink_mask_ & TS_AS_LAYOUT_CHANGE) sink->OnLayoutChange(TS_LC_CHANGE, kView);
        }
        sink->Release();
        if (!any && !pending_snapshot_) break;
    }
    flushing_ = false;
    Release();
}

// ---- the document ----------------------------------------------------------------------------

HRESULT Store::GetStatus(TS_STATUS* status) {
    if (!status) return E_INVALIDARG;
    // The app shows and hides the touch keyboard itself (showSoftInput, touch_keyboard.h), as
    // TS_SD_INPUTPANEMANUALDISPLAYENABLE tells Windows; the touch keyboard's suggestions and
    // corrections follow the field.
    status->dwDynamicFlags = TS_SD_INPUTPANEMANUALDISPLAYENABLE | (read_only() ? TS_SD_READONLY : 0);
    status->dwStaticFlags = TS_SS_NOHIDDENTEXT | (policy_.autocorrect ? TS_SS_TKBAUTOCORRECTENABLE : 0) |
                            (policy_.prediction ? TS_SS_TKBPREDICTIONENABLE : 0);
    return S_OK;
}

HRESULT Store::QueryInsert(LONG start, LONG end, ULONG, LONG* result_start, LONG* result_end) {
    if (!result_start || !result_end) return E_INVALIDARG;
    if (start < 0 || end < start || end > doc_.length()) return E_INVALIDARG;
    // Where asked: an editor puts text where it is put (a read-only one refuses it in SetText).
    *result_start = start;
    *result_end = end;
    return S_OK;
}

HRESULT Store::GetSelection(ULONG index, ULONG count, TS_SELECTION_ACP* out, ULONG* fetched) {
    if (!out || !fetched) return E_INVALIDARG;
    *fetched = 0;
    if (!connected_) return E_UNEXPECTED;
    if (!can_read()) return TS_E_NOLOCK;
    if (index != 0 && index != static_cast<ULONG>(TS_DEFAULT_SELECTION)) return TS_E_NOSELECTION;
    if (count == 0) return S_OK;
    out[0].acpStart = doc_.sel_start();
    out[0].acpEnd = doc_.sel_end();
    out[0].style.ase = doc_.sel_start() == doc_.sel_end() ? TS_AE_NONE : doc_.caret_at_start() ? TS_AE_START : TS_AE_END;
    out[0].style.fInterimChar = interim_ ? TRUE : FALSE;
    *fetched = 1;
    return S_OK;
}

HRESULT Store::SetSelection(ULONG count, const TS_SELECTION_ACP* selection) {
    if (!connected_) return E_UNEXPECTED;
    if (!can_write()) return TS_E_NOLOCK;
    if (count == 0 || !selection) return E_INVALIDARG;
    if (!doc_.select(selection->acpStart, selection->acpEnd, selection->style.ase == TS_AE_START)) {
        return TS_E_INVALIDPOS;
    }
    interim_ = selection->style.fInterimChar != FALSE;
    return S_OK;
}

HRESULT Store::GetText(LONG start, LONG end, WCHAR* plain, ULONG plain_wanted, ULONG* plain_got, TS_RUNINFO* runs,
                       ULONG runs_wanted, ULONG* runs_got, LONG* next) {
    if (!plain_got || !runs_got || !next) return E_INVALIDARG;
    *plain_got = *runs_got = 0;
    if (!connected_) return E_UNEXPECTED;
    if (!can_read()) return TS_E_NOLOCK;
    const LONG len = doc_.length();
    if (end == -1) end = len;
    if (start < 0 || start > len || end < start || end > len) return TS_E_INVALIDPOS;
    const ULONG span = static_cast<ULONG>(end - start);
    const ULONG count = plain ? std::min(span, plain_wanted) : span;
    if (plain && count > 0) std::memcpy(plain, doc_.text().data() + start, count * sizeof(WCHAR));
    if (plain) *plain_got = count;
    if (runs && runs_wanted > 0 && count > 0) {
        runs[0].uCount = count;  // all of it plain text: an editor has no hidden or embedded text
        runs[0].type = TS_RT_PLAIN;
        *runs_got = 1;
    }
    *next = start + static_cast<LONG>(count);
    return S_OK;
}

HRESULT Store::SetText(DWORD, LONG start, LONG end, const WCHAR* text, ULONG length, TS_TEXTCHANGE* change) {
    if (!connected_) return E_UNEXPECTED;
    if (!can_write()) return TS_E_NOLOCK;
    if (read_only()) return TS_E_READONLY;
    if (!text && length > 0) return E_INVALIDARG;
    const std::u16string_view s(reinterpret_cast<const char16_t*>(text), length);
    if (!doc_.replace(start, end, s)) return TS_E_INVALIDPOS;
    if (change) *change = TS_TEXTCHANGE{start, end, start + static_cast<LONG>(length)};
    return S_OK;
}

HRESULT Store::InsertTextAtSelection(DWORD flags, const WCHAR* text, ULONG length, LONG* out_start, LONG* out_end,
                                     TS_TEXTCHANGE* change) {
    if (!connected_) return E_UNEXPECTED;
    const LONG start = doc_.sel_start(), end = doc_.sel_end();
    if (flags & TS_IAS_QUERYONLY) {
        if (!can_read()) return TS_E_NOLOCK;
        if (out_start) *out_start = start;
        if (out_end) *out_end = start + static_cast<LONG>(length);
        return S_OK;
    }
    if (!can_write()) return TS_E_NOLOCK;
    if (read_only()) return TS_E_READONLY;
    if (!text && length > 0) return E_INVALIDARG;
    if (!doc_.replace(start, end, std::u16string_view(reinterpret_cast<const char16_t*>(text), length))) {
        return TS_E_INVALIDPOS;
    }
    // The caret goes after what was inserted, as typing leaves it.
    const LONG after = start + static_cast<LONG>(length);
    doc_.select(after, after, false);
    if (!(flags & TS_IAS_NOQUERY)) {
        if (out_start) *out_start = start;
        if (out_end) *out_end = after;
    }
    if (change) *change = TS_TEXTCHANGE{start, end, after};
    return S_OK;
}

HRESULT Store::GetFormattedText(LONG start, LONG end, IDataObject** out) {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    if (!connected_) return E_UNEXPECTED;
    if (!can_read()) return TS_E_NOLOCK;
    const LONG len = doc_.length();
    if (end == -1) end = len;
    if (start < 0 || end < start || end > len) return TS_E_INVALIDPOS;
    *out = new PlainText(doc_.text().substr(static_cast<size_t>(start), static_cast<size_t>(end - start)));
    return S_OK;
}

HRESULT Store::GetEmbedded(LONG, REFGUID, REFIID, IUnknown** out) {
    if (out) *out = nullptr;
    return E_NOTIMPL;  // an editor has no embedded objects
}

HRESULT Store::QueryInsertEmbedded(const GUID*, const FORMATETC*, BOOL* insertable) {
    if (!insertable) return E_INVALIDARG;
    *insertable = FALSE;
    return S_OK;
}

HRESULT Store::InsertEmbedded(DWORD, LONG, LONG, IDataObject*, TS_TEXTCHANGE*) { return E_NOTIMPL; }

HRESULT Store::InsertEmbeddedAtSelection(DWORD, IDataObject*, LONG*, LONG*, TS_TEXTCHANGE*) { return E_NOTIMPL; }

// ---- attributes: the field's input scope -----------------------------------------------------

void Store::QueueAttrs(ULONG count, const TS_ATTRID* attrs, DWORD flags) {
    requested_.clear();
    for (ULONG i = 0; attrs && i < count; ++i) {
        if (attrs[i] == kGUID_PROP_INPUTSCOPE) requested_.push_back(Requested{(flags & TS_ATTR_FIND_WANT_VALUE) != 0});
    }
}

HRESULT Store::RequestSupportedAttrs(DWORD flags, ULONG count, const TS_ATTRID* attrs) {
    if (count > 0 && !attrs) return E_INVALIDARG;
    QueueAttrs(count, attrs, flags);
    return S_OK;
}

HRESULT Store::RequestAttrsAtPosition(LONG position, ULONG count, const TS_ATTRID* attrs, DWORD) {
    if (count > 0 && !attrs) return E_INVALIDARG;
    if (position < 0 || position > doc_.length()) return TS_E_INVALIDPOS;
    QueueAttrs(count, attrs, TS_ATTR_FIND_WANT_VALUE);  // the same everywhere in the field
    return S_OK;
}

HRESULT Store::RequestAttrsTransitioningAtPosition(LONG position, ULONG, const TS_ATTRID*, DWORD) {
    if (position < 0 || position > doc_.length()) return TS_E_INVALIDPOS;
    requested_.clear();  // nothing starts or ends anywhere
    return S_OK;
}

HRESULT Store::FindNextAttrTransition(LONG start, LONG halt, ULONG, const TS_ATTRID*, DWORD, LONG* next, BOOL* found,
                                      LONG* found_offset) {
    if (!next || !found) return E_INVALIDARG;
    if (start < 0 || start > doc_.length()) return TS_E_INVALIDPOS;
    *next = halt;
    *found = FALSE;
    if (found_offset) *found_offset = 0;
    return S_OK;
}

HRESULT Store::RetrieveRequestedAttrs(ULONG count, TS_ATTRVAL* values, ULONG* fetched) {
    if (!values || !fetched) return E_INVALIDARG;
    *fetched = 0;
    ULONG i = 0;
    for (; i < count && i < requested_.size(); ++i) {
        values[i].idAttr = kGUID_PROP_INPUTSCOPE;
        values[i].dwOverlapId = 0;
        VariantInit(&values[i].varValue);
        if (requested_[i].value && scopes_) {
            values[i].varValue.vt = VT_UNKNOWN;
            values[i].varValue.punkVal = scopes_;
            scopes_->AddRef();
        }
    }
    requested_.erase(requested_.begin(), requested_.begin() + i);
    *fetched = i;
    return S_OK;
}

// ---- views: where the text is on the screen ---------------------------------------------------

HRESULT Store::GetEndACP(LONG* end) {
    if (!end) return E_INVALIDARG;
    if (!can_read()) return TS_E_NOLOCK;
    *end = doc_.length();
    return S_OK;
}

HRESULT Store::GetActiveView(TsViewCookie* view) {
    if (!view) return E_INVALIDARG;
    *view = kView;
    return S_OK;
}

HRESULT Store::GetACPFromPoint(TsViewCookie view, const POINT* point, DWORD flags, LONG* acp) {
    if (!point || !acp) return E_INVALIDARG;
    if (view != kView) return E_INVALIDARG;
    if (!can_read()) return TS_E_NOLOCK;
    float x = 0, y = 0;
    if (layout_.epoch != doc_.epoch() || !owner_->from_screen(*point, &x, &y)) return TS_E_INVALIDPOINT;
    const int32_t at = textinput::position_at(layout_, x, y, (flags & GXFPF_ROUND_NEAREST) != 0,
                                               (flags & GXFPF_NEAREST) != 0);
    if (at < 0) return TS_E_INVALIDPOINT;
    *acp = std::clamp<LONG>(at - doc_.offset(), 0, doc_.length());
    return S_OK;
}

HRESULT Store::GetTextExt(TsViewCookie view, LONG start, LONG end, RECT* rect, BOOL* clipped) {
    if (!rect || !clipped) return E_INVALIDARG;
    *rect = RECT{};
    *clipped = FALSE;
    if (view != kView) return E_INVALIDARG;
    if (!connected_) return E_UNEXPECTED;
    if (!can_read()) return TS_E_NOLOCK;
    if (start < 0 || end < start || end > doc_.length()) return TS_E_INVALIDPOS;
    const bool current = layout_.epoch == doc_.epoch();
    bool cut = false;
    std::optional<Box> box;
    if (current) box = textinput::extent(layout_, start + doc_.offset(), end + doc_.offset(), &cut);
    if (!box) {
        // The editor has not drawn this text yet (an edit on its way, or a layout pending):
        // the text service asks again on OnLayoutChange, which the next measurement sends.
        if (doc_.in_flight() || !current || layout_.acked < doc_.sent()) return TS_E_NOLAYOUT;
        // Measured, but not these characters (an editor with no Layout to ask, a Compose field):
        // the caret, else the editor, else nothing better than the window.
        if (layout_.has_caret) {
            box = layout_.caret;
        } else if (layout_.has_editor) {
            box = Box{layout_.editor.left, layout_.editor.bottom, layout_.editor.left, layout_.editor.bottom};
        }
    }
    if (!box || !owner_->to_screen(*box, rect)) {
        HWND hwnd = owner_->window();
        RECT c{};
        if (!hwnd || !GetClientRect(hwnd, &c)) return TS_E_NOLAYOUT;
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&c), 2);
        *rect = RECT{std::min(c.left, c.right), c.bottom, std::min(c.left, c.right) + 1, c.bottom};
        return S_OK;
    }
    if (rect->right <= rect->left) rect->right = rect->left + 1;  // a caret: a sliver, not nothing
    *clipped = cut ? TRUE : FALSE;
    return S_OK;
}

HRESULT Store::GetScreenExt(TsViewCookie view, RECT* rect) {
    if (!rect) return E_INVALIDARG;
    *rect = RECT{};
    if (view != kView) return E_INVALIDARG;
    if (layout_.epoch == doc_.epoch() && layout_.has_editor && owner_->to_screen(layout_.editor, rect)) return S_OK;
    HWND hwnd = owner_->window();
    RECT c{};
    if (!hwnd || !GetClientRect(hwnd, &c)) return E_FAIL;
    MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&c), 2);
    *rect = RECT{std::min(c.left, c.right), c.top, std::max(c.left, c.right), c.bottom};
    return S_OK;
}

HRESULT Store::GetWnd(TsViewCookie view, HWND* hwnd) {
    if (!hwnd) return E_INVALIDARG;
    *hwnd = view == kView ? owner_->window() : nullptr;
    return view == kView ? S_OK : E_INVALIDARG;
}

// ---- compositions ----------------------------------------------------------------------------

HRESULT Store::OnStartComposition(ITfCompositionView* view, BOOL* ok) {
    if (!ok) return E_INVALIDARG;
    // A field that takes no composition (a password) refuses it; its keyboard is disabled for
    // text services too (tsf_session.cpp), so this is the second lock on that door.
    *ok = connected_ && policy_.compose ? TRUE : FALSE;
    if (!*ok || !view) return S_OK;
    if (composition_) composition_->Release();
    composition_ = view;
    composition_->AddRef();
    ReadComposition();
    if (lock_ == 0) EndOfChange();
    return S_OK;
}

HRESULT Store::OnUpdateComposition(ITfCompositionView* view, ITfRange* range) {
    if (!connected_ || view != composition_) return S_OK;
    LONG s = 0, e = 0;
    if (range && range_extent(range, &s, &e)) doc_.compose(s, e);
    if (lock_ == 0) EndOfChange();
    return S_OK;
}

HRESULT Store::OnEndComposition(ITfCompositionView* view) {
    if (!connected_ || view != composition_) return S_OK;
    composition_->Release();
    composition_ = nullptr;
    doc_.compose(-1, -1);
    if (lock_ == 0) EndOfChange();
    return S_OK;
}

void Store::ReadComposition() {
    if (!composition_) return;
    ITfRange* range = nullptr;
    LONG s = 0, e = 0;
    if (SUCCEEDED(composition_->GetRange(&range)) && range) {
        if (range_extent(range, &s, &e)) doc_.compose(s, e);
        range->Release();
    }
}

HRESULT Store::OnEndEdit(ITfContext* context, TfEditCookie cookie, ITfEditRecord*) {
    if (!connected_ || !context) return S_OK;
    ReadComposition();
    ReadAttributes(context, cookie);
    return S_OK;
}

// The composition's look: each range of GUID_PROP_ATTRIBUTE over it names a display attribute
// (a GUID atom), which the display attribute manager describes.
void Store::ReadAttributes(ITfContext* context, TfEditCookie cookie) {
    ITfCategoryMgr* categories = owner_->categories();
    ITfDisplayAttributeMgr* attributes = owner_->display_attributes();
    if (!composition_ || !categories || !attributes) return;
    ITfRange* whole = nullptr;
    ITfProperty* property = nullptr;
    IEnumTfRanges* ranges = nullptr;
    std::vector<textinput::Run> runs;
    if (SUCCEEDED(composition_->GetRange(&whole)) && whole &&
        SUCCEEDED(context->GetProperty(GUID_PROP_ATTRIBUTE, &property)) && property &&
        SUCCEEDED(property->EnumRanges(cookie, &ranges, whole)) && ranges) {
        ITfRange* range = nullptr;
        ULONG got = 0;
        while (ranges->Next(1, &range, &got) == S_OK && got == 1) {
            VARIANT value;
            VariantInit(&value);
            LONG s = 0, e = 0;
            if (SUCCEEDED(property->GetValue(cookie, range, &value)) && value.vt == VT_I4 &&
                range_extent(range, &s, &e)) {
                GUID guid{};
                ITfDisplayAttributeInfo* info = nullptr;
                TF_DISPLAYATTRIBUTE da{};
                if (SUCCEEDED(categories->GetGUID(static_cast<TfGuidAtom>(value.lVal), &guid)) &&
                    SUCCEEDED(attributes->GetDisplayAttributeInfo(guid, &info, nullptr)) && info &&
                    SUCCEEDED(info->GetAttributeInfo(&da))) {
                    runs.push_back(textinput::style_run(plain(da), s, e,
                                                        [this](int i) { return owner_->sys_color(i); }));
                }
                if (info) info->Release();
            }
            VariantClear(&value);
            range->Release();
        }
    }
    if (ranges) ranges->Release();
    if (property) property->Release();
    if (whole) whole->Release();
    doc_.style(std::move(runs));
}

}  // namespace winapk::host::tsf
