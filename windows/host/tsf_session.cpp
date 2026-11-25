// tsf_session.cpp -- see tsf_session.h.

#include "tsf_session.h"

#include <imm.h>
#include <inputscope.h>

#include <cstdio>
#include <vector>

namespace winapk::host::tsf {

namespace {

// IMM32 and SetInputScopes, looked up when first needed: the tests that link this file need
// neither, and a Windows without them (Wine's partial msctf) only loses what they add.
struct Imm32 {
    BOOL(WINAPI* associate_ex)(HWND, HIMC, DWORD) = nullptr;
};

const Imm32& imm32() {
    static const Imm32 api = [] {
        Imm32 a;
        if (HMODULE dll = LoadLibraryW(L"imm32.dll")) {
            a.associate_ex = reinterpret_cast<decltype(a.associate_ex)>(GetProcAddress(dll, "ImmAssociateContextEx"));
        }
        return a;
    }();
    return api;
}

using SetInputScopesFn = HRESULT(WINAPI*)(HWND, const InputScope*, UINT, WCHAR**, UINT, WCHAR*, WCHAR*);

SetInputScopesFn set_input_scopes() {
    static const SetInputScopesFn fn = [] {
        HMODULE dll = LoadLibraryW(L"msctf.dll");
        return dll ? reinterpret_cast<SetInputScopesFn>(GetProcAddress(dll, "SetInputScopes")) : nullptr;
    }();
    return fn;
}

template <typename T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

}  // namespace

std::unique_ptr<Session> Session::Create(HWND window, SessionHost host, HRESULT* why) {
    std::unique_ptr<Session> s(new Session(window, std::move(host)));
    const HRESULT hr = s->Start();
    if (why) *why = hr;
    if (FAILED(hr)) return nullptr;
    return s;
}

HRESULT Session::Start() {
    HRESULT hr = CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
                                  reinterpret_cast<void**>(&threads_));
    if (FAILED(hr) || !threads_) return FAILED(hr) ? hr : E_NOINTERFACE;
    ITfThreadMgrEx* ex = nullptr;
    if (SUCCEEDED(threads_->QueryInterface(IID_ITfThreadMgrEx, reinterpret_cast<void**>(&ex))) && ex) {
        hr = ex->ActivateEx(&client_, 0);
        ex->Release();
    } else {
        hr = threads_->Activate(&client_);
    }
    if (FAILED(hr)) return hr;
    activated_ = true;
    if (FAILED(threads_->QueryInterface(IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&keys_)))) keys_ = nullptr;
    // Without these the composition keeps the editor's own look (an underline): not a failure.
    if (FAILED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
                                reinterpret_cast<void**>(&categories_)))) {
        categories_ = nullptr;
    }
    if (FAILED(CoCreateInstance(CLSID_TF_DisplayAttributeMgr, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ITfDisplayAttributeMgr, reinterpret_cast<void**>(&attributes_)))) {
        attributes_ = nullptr;
    }
    return S_OK;
}

Session::~Session() {
    Blur();
    release(attributes_);
    release(categories_);
    release(keys_);
    if (threads_ && activated_) threads_->Deactivate();
    release(threads_);
}

bool Session::Focus(const textinput::FieldPolicy& policy, const textinput::Snapshot& snapshot) {
    Blur();
    if (!policy.document || !threads_) return false;
    ITfDocumentMgr* document = nullptr;
    if (FAILED(threads_->CreateDocumentMgr(&document)) || !document) return false;
    Store* store = new Store(this, policy, snapshot);
    ITfContext* context = nullptr;
    TfEditCookie cookie = 0;
    HRESULT hr = document->CreateContext(client_, 0, static_cast<ITextStoreACP*>(store), &context, &cookie);
    if (SUCCEEDED(hr) && context) hr = document->Push(context);
    if (FAILED(hr) || !context) {
        std::fprintf(stderr, "[win.apk] text input: TSF refused the editor's document (0x%08lx)\n",
                     static_cast<unsigned long>(hr));
        store->Disconnect();
        release(context);
        document->Release();
        store->Release();
        return false;
    }
    store_ = store;
    document_ = document;
    context_ = context;
    // The composition's look, as the text services set it (Store::OnEndEdit).
    ITfSource* source = nullptr;
    if (SUCCEEDED(context->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source))) && source) {
        if (FAILED(source->AdviseSink(IID_ITfTextEditSink, static_cast<ITfTextEditSink*>(store), &edit_cookie_))) {
            edit_cookie_ = TF_INVALID_COOKIE;
        }
        source->Release();
    }
    if (!policy.compose) {
        // No text service composes in this field, and no IMM32 input method either.
        ITfCompartmentMgr* compartments = nullptr;
        ITfCompartment* disabled = nullptr;
        if (SUCCEEDED(context->QueryInterface(IID_ITfCompartmentMgr, reinterpret_cast<void**>(&compartments))) &&
            compartments &&
            SUCCEEDED(compartments->GetCompartment(GUID_COMPARTMENT_KEYBOARD_DISABLED, &disabled)) && disabled) {
            VARIANT on;
            VariantInit(&on);
            on.vt = VT_I4;
            on.lVal = 1;
            disabled->SetValue(client_, &on);
        }
        release(disabled);
        release(compartments);
        if (imm32().associate_ex) imm_off_ = imm32().associate_ex(window_, nullptr, 0) != FALSE;
    }
    if (SetInputScopesFn set = set_input_scopes()) {
        std::vector<InputScope> scopes;
        for (int32_t s : policy.scopes) scopes.push_back(static_cast<InputScope>(s));
        if (scopes.empty()) scopes.push_back(IS_DEFAULT);
        scoped_ = SUCCEEDED(set(window_, scopes.data(), static_cast<UINT>(scopes.size()), nullptr, 0, nullptr,
                                nullptr));
    }
    ITfDocumentMgr* previous = nullptr;
    threads_->AssociateFocus(window_, document, &previous);
    release(previous);
    // AssociateFocus moves TSF's focus when the window next takes the keyboard; it has it now.
    if (GetFocus() == window_) threads_->SetFocus(document);
    return true;
}

void Session::Blur() {
    if (!store_) return;
    ITfDocumentMgr* previous = nullptr;
    threads_->AssociateFocus(window_, nullptr, &previous);
    release(previous);
    if (edit_cookie_ != TF_INVALID_COOKIE && context_) {
        ITfSource* source = nullptr;
        if (SUCCEEDED(context_->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source))) && source) {
            source->UnadviseSink(edit_cookie_);
            source->Release();
        }
    }
    edit_cookie_ = TF_INVALID_COOKIE;
    // The editor is gone: nothing more reaches it, even from a composition TSF ends now.
    store_->Disconnect();
    if (document_) document_->Pop(TF_POPF_ALL);
    release(context_);
    release(document_);
    store_->Release();
    store_ = nullptr;
    if (imm_off_ && imm32().associate_ex) imm32().associate_ex(window_, nullptr, IACE_DEFAULT);
    imm_off_ = false;
    if (scoped_) {
        if (SetInputScopesFn set = set_input_scopes()) {
            const InputScope none = IS_DEFAULT;
            set(window_, &none, 1, nullptr, 0, nullptr, nullptr);
        }
    }
    scoped_ = false;
}

void Session::Update(const textinput::Snapshot& snapshot) {
    if (store_ && snapshot.epoch == store_->document().epoch()) store_->Update(snapshot);
}

void Session::Relayout(textinput::Layout layout) {
    if (store_ && layout.epoch == store_->document().epoch()) store_->Relayout(std::move(layout));
}

void Session::Moved() {
    if (store_) store_->Moved();
}

bool Session::Key(const MSG& m) {
    if (!keys_ || !store_ || m.hwnd != window_ || m.wParam == VK_PROCESSKEY) return false;
    BOOL eaten = FALSE;
    if (m.message == WM_KEYDOWN) {
        if (FAILED(keys_->TestKeyDown(m.wParam, m.lParam, &eaten)) || !eaten) return false;
        eaten = FALSE;
        return SUCCEEDED(keys_->KeyDown(m.wParam, m.lParam, &eaten)) && eaten;
    }
    if (m.message == WM_KEYUP) {
        if (FAILED(keys_->TestKeyUp(m.wParam, m.lParam, &eaten)) || !eaten) return false;
        eaten = FALSE;
        return SUCCEEDED(keys_->KeyUp(m.wParam, m.lParam, &eaten)) && eaten;
    }
    return false;
}

void Session::edit(const textinput::Edit& e) {
    if (host_.edit) host_.edit(e);
}

bool Session::to_screen(const textinput::Box& frame, RECT* screen) {
    return host_.to_screen && host_.to_screen(frame, screen);
}

bool Session::from_screen(POINT screen, float* x, float* y) {
    return host_.from_screen && host_.from_screen(screen, x, y);
}

uint32_t Session::sys_color(int index) {
    const COLORREF c = GetSysColor(index);
    return 0xff000000u | (static_cast<uint32_t>(GetRValue(c)) << 16) | (static_cast<uint32_t>(GetGValue(c)) << 8) |
           GetBValue(c);
}

}  // namespace winapk::host::tsf
