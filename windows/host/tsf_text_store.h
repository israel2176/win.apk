// Adapted from the other runtime (https://example.invalid/other-runtime), compat/input/text_store.h and
// compat/input/text_document.h, Copyright (C) 2026 its authors,
// SPDX-License-Identifier: Apache-2.0. Modified for Win.apk.
//
// tsf_text_store.h -- the focused Android editor as a Text Services Framework document.
//
// Windows' input methods (the Japanese, Chinese and Korean IMEs, handwriting, dictation, the
// touch keyboard's suggestions) are TSF text services: they read and write the focused
// window's document through ITextStoreACP. The document here is a copy of the focused
// editor's InputConnection (its text, selection and caret, HostTextInput.java on the editor's
// thread), updated whenever the editor changes; what a text service writes goes back to the
// editor as an input method's calls (setComposingRegion, setComposingText,
// finishComposingText, setSelection, a replacement), each with the editor's epoch and a
// revision so that a copy from before the edit cannot undo it. Positions are UTF-16 units,
// as both Android's and TSF's are. Nothing here knows about the JVM, so the COM contract can
// be tested on its own (windows/test/tsf_test.cpp).

#pragma once

#include <windows.h>
#include <msctf.h>
#include <olectl.h>
#include <textstor.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace winapk::host::tsf {

// A copy of the focused editor (ExtractedText): the text from `offset` on, the selection in it
// (reversed: the caret is at start), the editor's epoch (a new editor, a new epoch) and the
// newest of this store's edits the editor had applied when the copy was made.
struct TextDocument {
    std::u16string text;
    int32_t start = 0, end = 0;
    int32_t offset = 0;
    int32_t epoch = 0, applied_revision = 0;
    bool available = false;  // false: no editor takes compositions (none, or a password field)
    bool reversed = false;
};

// An edit a text service made, for the editor. Positions are the editor's (offset added).
struct DocumentEdit {
    enum Kind { kReplace, kSelect, kCompositionRange, kCompositionEnd, kCompositionText };
    Kind kind = kReplace;
    std::u16string text;
    int32_t start = 0, end = 0;
    int32_t epoch = 0, revision = 0;
};

// Where the editor drew a character, in client pixels.
struct TextCharacterBounds {
    int32_t index = -1, left = 0, top = 0, right = 0, bottom = 0;
    bool clipped = false;
    bool operator==(const TextCharacterBounds& other) const {
        return index == other.index && left == other.left && top == other.top && right == other.right &&
               bottom == other.bottom && clipped == other.clipped;
    }
};

// The characters of `text`, which starts at editor index `start`, as the editor laid them out.
struct TextGeometry {
    int32_t epoch = 0, start = 0;
    std::u16string text;
    std::vector<TextCharacterBounds> characters;  // absolute UTF-16 indices, in order
};

// All methods run on the window's thread (an STA). The owner hands it copies of the editor
// and schedules Pump() through its message loop. Notifications never echo TSF's own edits.
class TextStore final : public ITextStoreACP, public ITfContextOwnerCompositionSink {
public:
    using Emit = std::function<void(DocumentEdit)>;
    using Schedule = std::function<void()>;
    TextStore(HWND window, Emit emit, Schedule schedule);
    void Update(TextDocument document);
    // The caret, in screen pixels (empty: unknown).
    void Caret(const RECT& screen);
    void Geometry(TextGeometry geometry);
    void Pump();
    void Disconnect();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
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
    HRESULT STDMETHODCALLTYPE GetWnd(TsViewCookie, HWND*) override;
    HRESULT STDMETHODCALLTYPE OnStartComposition(ITfCompositionView*, BOOL*) override;
    HRESULT STDMETHODCALLTYPE OnUpdateComposition(ITfCompositionView*, ITfRange*) override;
    HRESULT STDMETHODCALLTYPE OnEndComposition(ITfCompositionView*) override;

private:
    ~TextStore();
    bool ReadLock() const;
    bool WriteLock() const;
    bool Range(LONG, LONG) const;
    bool FitsReplacement(LONG, LONG, ULONG) const;
    bool GeometryMatches() const;
    void Publish(DocumentEdit);
    void Apply(TextDocument);
    HWND window_;
    Emit emit_;
    Schedule schedule_;
    LONG references_ = 1;
    ITextStoreACPSink* sink_ = nullptr;
    DWORD sink_mask_ = 0, lock_ = 0, pending_lock_ = 0;
    bool pumping_ = false, composing_ = false, pending_layout_ = false;
    int32_t revision_ = 0;
    LONG composition_start_ = 0, composition_end_ = 0;
    TextDocument document_;
    TextGeometry geometry_;
    std::optional<TextDocument> pending_document_;
    RECT caret_ = {};
};

}  // namespace winapk::host::tsf
