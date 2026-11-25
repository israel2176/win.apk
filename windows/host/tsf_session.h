// tsf_session.h -- the window thread's Text Services Framework: its thread manager, and one
// document manager and context over the focused editor's text store (tsf_store.h).
//
// Made when an editor first takes the focus, never before (a run with no text field starts no
// TSF), and kept for the window's life: each editor that takes the focus gets a document of its
// own, associated with the window (ITfThreadMgr::AssociateFocus), so Windows' text services
// follow the focus between the app and other windows by themselves; the document goes when the
// editor does. For a field that takes no composition (a password) the context's
// GUID_COMPARTMENT_KEYBOARD_DISABLED is set and IMM32 is turned off for the window while it has
// the focus: no input method composes in it. The field's input scopes are also set on the window
// (SetInputScopes), for input methods that still speak IMM32 and the touch keyboard.
//
// Keys reach the text services before the app (Key): an input method's key is its own.

#pragma once

#include <windows.h>
#include <msctf.h>

#include <functional>
#include <memory>

#include "text_input_model.h"
#include "tsf_store.h"

namespace winapk::host::tsf {

struct SessionHost {
    std::function<void(const textinput::Edit&)> edit;
    std::function<bool(const textinput::Box&, RECT*)> to_screen;
    std::function<bool(POINT, float*, float*)> from_screen;
};

class Session final : private StoreOwner {
public:
    // On the window's thread, an STA. Null, with the failing HRESULT in *why, when TSF is not
    // there or will not activate.
    static std::unique_ptr<Session> Create(HWND window, SessionHost host, HRESULT* why);
    ~Session() override;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // An editor of kind `policy` took the focus, as `snapshot` shows it. False when no document
    // was made for it (TYPE_NULL, or TSF refused): input methods then go through IMM32.
    bool Focus(const textinput::FieldPolicy& policy, const textinput::Snapshot& snapshot);
    // The editor lost the focus: its document goes, and the window is as it was.
    void Blur();
    bool focused() const { return store_ != nullptr; }
    int32_t epoch() const { return store_ ? store_->document().epoch() : -1; }

    void Update(const textinput::Snapshot& snapshot);
    void Relayout(textinput::Layout layout);
    void Moved();

    // A key message for the text services first: true when one of them took it (the app must
    // then not see it).
    bool Key(const MSG& m);

private:
    Session(HWND window, SessionHost host) : window_(window), host_(std::move(host)) {}
    HRESULT Start();
    // StoreOwner
    HWND window() override { return window_; }
    void edit(const textinput::Edit& e) override;
    bool to_screen(const textinput::Box& frame, RECT* screen) override;
    bool from_screen(POINT screen, float* x, float* y) override;
    uint32_t sys_color(int index) override;
    ITfCategoryMgr* categories() override { return categories_; }
    ITfDisplayAttributeMgr* display_attributes() override { return attributes_; }

    HWND window_;
    SessionHost host_;
    ITfThreadMgr* threads_ = nullptr;
    ITfKeystrokeMgr* keys_ = nullptr;
    ITfCategoryMgr* categories_ = nullptr;
    ITfDisplayAttributeMgr* attributes_ = nullptr;
    TfClientId client_ = 0;
    bool activated_ = false;
    // The focused editor's.
    Store* store_ = nullptr;
    ITfDocumentMgr* document_ = nullptr;
    ITfContext* context_ = nullptr;
    DWORD edit_cookie_ = TF_INVALID_COOKIE;
    bool imm_off_ = false;
    bool scoped_ = false;
};

}  // namespace winapk::host::tsf
