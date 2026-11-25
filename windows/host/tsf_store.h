// tsf_store.h -- the focused Android editor as a Text Services Framework text store.
//
// Windows' input methods are TSF text services: they read and write the focused document
// through the text store its application implements (ITextStoreACP, and ITextStoreACP2 from
// Windows 8, which is the same without GetWnd; TSF takes whichever it asks for), only while
// they hold one of its locks. The store here keeps the document as text_input_model.h's
// Document over what the editor side sends, and is a thin COM face on it:
//
//   - locks as TSF defines them: one at a time; a synchronous request while one is held fails
//     with TS_E_SYNCHRONOUS; an asynchronous one is queued (TS_S_ASYNC) and granted, in order,
//     when the lock held ends, which is how a text service holding a read lock gets to write;
//   - every lock that wrote becomes one Edit for the editor when it ends (with the composition
//     as TSF reports it through ITfContextOwnerCompositionSink, and its look as the text
//     services' display attributes, read in ITfTextEditSink::OnEndEdit);
//   - the editor's own changes (a key typed with no text service, the app's setText, a filter)
//     reach the text services as OnTextChange / OnSelectionChange, never inside a lock and
//     never for a change they made themselves; a moved or relaid-out editor as OnLayoutChange;
//   - GetTextExt and GetACPFromPoint from the boxes the editor's real Layout drew its
//     characters in (right to left too), so a candidate window sits under the composition;
//   - the field's kind (EditorInfo.inputType) as the document's input scopes
//     (GUID_PROP_INPUTSCOPE, an ITfInputScope) and the touch keyboard's flags in GetStatus;
//     a password is a read-only, empty document.
//
// Everything runs on the window's thread (an STA). The owner (tsf_session.h) gives the store
// its edits' destination and the window's coordinates.

#pragma once

#include <windows.h>
#include <msctf.h>
#include <textstor.h>

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "text_input_model.h"

namespace winapk::host::tsf {

// ITextStoreACP2 (textstor.h of the Windows 8 SDK; not in every MinGW's): ITextStoreACP's
// methods in the same order, without GetWnd.
struct TextStoreACP2 : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE AdviseSink(REFIID, IUnknown*, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnadviseSink(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE RequestLock(DWORD, HRESULT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStatus(TS_STATUS*) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryInsert(LONG, LONG, ULONG, LONG*, LONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSelection(ULONG, ULONG, TS_SELECTION_ACP*, ULONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSelection(ULONG, const TS_SELECTION_ACP*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetText(LONG, LONG, WCHAR*, ULONG, ULONG*, TS_RUNINFO*, ULONG, ULONG*,
                                              LONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetText(DWORD, LONG, LONG, const WCHAR*, ULONG, TS_TEXTCHANGE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFormattedText(LONG, LONG, IDataObject**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetEmbedded(LONG, REFGUID, REFIID, IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryInsertEmbedded(const GUID*, const FORMATETC*, BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE InsertEmbedded(DWORD, LONG, LONG, IDataObject*, TS_TEXTCHANGE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE InsertTextAtSelection(DWORD, const WCHAR*, ULONG, LONG*, LONG*,
                                                            TS_TEXTCHANGE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE InsertEmbeddedAtSelection(DWORD, IDataObject*, LONG*, LONG*,
                                                                TS_TEXTCHANGE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE RequestSupportedAttrs(DWORD, ULONG, const TS_ATTRID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE RequestAttrsAtPosition(LONG, ULONG, const TS_ATTRID*, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE RequestAttrsTransitioningAtPosition(LONG, ULONG, const TS_ATTRID*, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindNextAttrTransition(LONG, LONG, ULONG, const TS_ATTRID*, DWORD, LONG*,
                                                             BOOL*, LONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE RetrieveRequestedAttrs(ULONG, TS_ATTRVAL*, ULONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetEndACP(LONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetActiveView(TsViewCookie*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetACPFromPoint(TsViewCookie, const POINT*, DWORD, LONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTextExt(TsViewCookie, LONG, LONG, RECT*, BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetScreenExt(TsViewCookie, RECT*) = 0;
};
extern const IID kIID_ITextStoreACP2;
extern const GUID kGUID_PROP_INPUTSCOPE;

// What the store needs from the window it serves.
class StoreOwner {
public:
    virtual ~StoreOwner() = default;
    virtual HWND window() = 0;
    // An edit the text services made, for the editor.
    virtual void edit(const textinput::Edit& e) = 0;
    // App frame pixels <-> screen pixels; false when the window shows no frame.
    virtual bool to_screen(const textinput::Box& frame, RECT* screen) = 0;
    virtual bool from_screen(POINT screen, float* x, float* y) = 0;
    // GetSysColor as ARGB, for display attributes that name a system color.
    virtual uint32_t sys_color(int index) = 0;
    // The managers that turn a range's GUID_PROP_ATTRIBUTE into a TF_DISPLAYATTRIBUTE (may be null).
    virtual ITfCategoryMgr* categories() { return nullptr; }
    virtual ITfDisplayAttributeMgr* display_attributes() { return nullptr; }
};

class Store final : public ITextStoreACP,
                    public TextStoreACP2,
                    public ITfContextOwnerCompositionSink,
                    public ITfTextEditSink {
public:
    static constexpr TsViewCookie kView = 1;  // the editor: one view

    // A store for one editor, of the kind `policy` says, starting as `initial` shows it.
    Store(StoreOwner* owner, textinput::FieldPolicy policy, const textinput::Snapshot& initial);

    // The editor's newest copy of itself, and where it drew its text: taken now when no lock is
    // held, else when the lock ends; the text services hear of what changed.
    void Update(const textinput::Snapshot& snapshot);
    void Relayout(textinput::Layout layout);
    // The window moved or was resized: every screen position changed.
    void Moved();
    // The editor is gone: no more notifications, and every call fails from now on.
    void Disconnect();

    const textinput::Document& document() const { return doc_; }
    const textinput::FieldPolicy& policy() const { return policy_; }
    bool locked() const { return lock_ != 0; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    // ITextStoreACP and ITextStoreACP2
    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID, IUnknown*, DWORD) override;
    HRESULT STDMETHODCALLTYPE UnadviseSink(IUnknown*) override;
    HRESULT STDMETHODCALLTYPE RequestLock(DWORD, HRESULT*) override;
    HRESULT STDMETHODCALLTYPE GetStatus(TS_STATUS*) override;
    HRESULT STDMETHODCALLTYPE QueryInsert(LONG, LONG, ULONG, LONG*, LONG*) override;
    HRESULT STDMETHODCALLTYPE GetSelection(ULONG, ULONG, TS_SELECTION_ACP*, ULONG*) override;
    HRESULT STDMETHODCALLTYPE SetSelection(ULONG, const TS_SELECTION_ACP*) override;
    HRESULT STDMETHODCALLTYPE GetText(LONG, LONG, WCHAR*, ULONG, ULONG*, TS_RUNINFO*, ULONG, ULONG*, LONG*) override;
    HRESULT STDMETHODCALLTYPE SetText(DWORD, LONG, LONG, const WCHAR*, ULONG, TS_TEXTCHANGE*) override;
    HRESULT STDMETHODCALLTYPE GetFormattedText(LONG, LONG, IDataObject**) override;
    HRESULT STDMETHODCALLTYPE GetEmbedded(LONG, REFGUID, REFIID, IUnknown**) override;
    HRESULT STDMETHODCALLTYPE QueryInsertEmbedded(const GUID*, const FORMATETC*, BOOL*) override;
    HRESULT STDMETHODCALLTYPE InsertEmbedded(DWORD, LONG, LONG, IDataObject*, TS_TEXTCHANGE*) override;
    HRESULT STDMETHODCALLTYPE InsertTextAtSelection(DWORD, const WCHAR*, ULONG, LONG*, LONG*,
                                                    TS_TEXTCHANGE*) override;
    HRESULT STDMETHODCALLTYPE InsertEmbeddedAtSelection(DWORD, IDataObject*, LONG*, LONG*, TS_TEXTCHANGE*) override;
    HRESULT STDMETHODCALLTYPE RequestSupportedAttrs(DWORD, ULONG, const TS_ATTRID*) override;
    HRESULT STDMETHODCALLTYPE RequestAttrsAtPosition(LONG, ULONG, const TS_ATTRID*, DWORD) override;
    HRESULT STDMETHODCALLTYPE RequestAttrsTransitioningAtPosition(LONG, ULONG, const TS_ATTRID*, DWORD) override;
    HRESULT STDMETHODCALLTYPE FindNextAttrTransition(LONG, LONG, ULONG, const TS_ATTRID*, DWORD, LONG*, BOOL*,
                                                     LONG*) override;
    HRESULT STDMETHODCALLTYPE RetrieveRequestedAttrs(ULONG, TS_ATTRVAL*, ULONG*) override;
    HRESULT STDMETHODCALLTYPE GetEndACP(LONG*) override;
    HRESULT STDMETHODCALLTYPE GetActiveView(TsViewCookie*) override;
    HRESULT STDMETHODCALLTYPE GetACPFromPoint(TsViewCookie, const POINT*, DWORD, LONG*) override;
    HRESULT STDMETHODCALLTYPE GetTextExt(TsViewCookie, LONG, LONG, RECT*, BOOL*) override;
    HRESULT STDMETHODCALLTYPE GetScreenExt(TsViewCookie, RECT*) override;
    HRESULT STDMETHODCALLTYPE GetWnd(TsViewCookie, HWND*) override;  // ITextStoreACP only
    // ITfContextOwnerCompositionSink
    HRESULT STDMETHODCALLTYPE OnStartComposition(ITfCompositionView*, BOOL*) override;
    HRESULT STDMETHODCALLTYPE OnUpdateComposition(ITfCompositionView*, ITfRange*) override;
    HRESULT STDMETHODCALLTYPE OnEndComposition(ITfCompositionView*) override;
    // ITfTextEditSink
    HRESULT STDMETHODCALLTYPE OnEndEdit(ITfContext*, TfEditCookie, ITfEditRecord*) override;

private:
    ~Store();
    bool can_read() const { return (lock_ & TS_LF_READ) == TS_LF_READ; }
    bool can_write() const { return (lock_ & TS_LF_READWRITE) == TS_LF_READWRITE; }
    bool read_only() const { return policy_.hide_text; }
    HRESULT Grant(DWORD flags);
    void EndOfChange();  // a write lock ended, or the composition changed outside one
    void Flush();        // what is waiting for no lock to be held
    void ReadComposition();
    void ReadAttributes(ITfContext* context, TfEditCookie cookie);
    void QueueAttrs(ULONG count, const TS_ATTRID* attrs, DWORD flags);

    LONG refs_ = 1;
    StoreOwner* owner_;
    textinput::FieldPolicy policy_;
    textinput::Document doc_;
    textinput::Layout layout_;
    bool connected_ = true;
    ITextStoreACPSink* sink_ = nullptr;
    DWORD sink_mask_ = 0;
    DWORD lock_ = 0;
    std::deque<DWORD> queue_;
    bool flushing_ = false;
    std::optional<textinput::Snapshot> pending_snapshot_;
    std::optional<textinput::TextChange> pending_text_;
    bool pending_selection_ = false, pending_layout_ = false;
    ITfCompositionView* composition_ = nullptr;
    bool interim_ = false;  // the selection is an interim character (Korean: the syllable being built)
    struct Requested {
        bool value;
    };
    std::vector<Requested> requested_;  // GUID_PROP_INPUTSCOPE requests waiting for RetrieveRequestedAttrs
    IUnknown* scopes_ = nullptr;        // the ITfInputScope
};

}  // namespace winapk::host::tsf
