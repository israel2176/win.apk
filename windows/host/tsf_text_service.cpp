// Adapted from the other runtime (https://example.invalid/other-runtime), compat/input/text_service.cc,
// Copyright (C) 2026 its authors, SPDX-License-Identifier: Apache-2.0. Modified for Win.apk.
//
// tsf_text_service.cpp -- see tsf_text_service.h. Win.apk's changes: the namespace, and no
// test-service entry point (the other runtime's fixture DLL).

#include "tsf_text_service.h"
#include <objbase.h>
#include <utility>
namespace winapk::host::tsf {
std::shared_ptr<TextService> TextService::Attach(HWND window, TextDocument initial,
    TextStore::Emit emit, TextStore::Schedule schedule, HRESULT* result) {
  auto service = std::shared_ptr<TextService>(new TextService());
  service->window_ = window;
  service->store_ = new TextStore(window, std::move(emit), std::move(schedule));
  service->store_->Update(std::move(initial));
  auto setup = [&]() -> HRESULT {
    HRESULT status = CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfThreadMgr, reinterpret_cast<void**>(&service->manager_));
    if (FAILED(status)) return status;
    status = service->manager_->Activate(&service->client_);
    if (FAILED(status)) return status;
    service->activated_ = true;
    status = service->manager_->CreateDocumentMgr(&service->document_);
    if (FAILED(status)) return status;
    TfEditCookie cookie = 0;
    status = service->document_->CreateContext(service->client_, 0,
        static_cast<ITextStoreACP*>(service->store_), &service->context_, &cookie);
    if (FAILED(status)) return status;
    status = service->document_->Push(service->context_);
    if (FAILED(status)) return status;
    status = service->manager_->AssociateFocus(window, service->document_, &service->previous_);
    if (FAILED(status)) return status;
    service->associated_ = true;
    service->manager_->QueryInterface(IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&service->keys_));
    if (GetFocus() == window) service->Focus(true);
    return S_OK;
  };
  HRESULT status = setup();
  if (result != nullptr) *result = status;
  return SUCCEEDED(status) ? service : nullptr;
}
TextService::~TextService() {
  Retire();
  if (previous_ != nullptr) previous_->Release();
  if (document_ != nullptr) document_->Pop(TF_POPF_ALL);
  if (context_ != nullptr) context_->Release();
  if (document_ != nullptr) document_->Release();
  if (keys_ != nullptr) keys_->Release();
  if (activated_) manager_->Deactivate();
  if (manager_ != nullptr) manager_->Release();
  if (store_ != nullptr) store_->Release();
}
void TextService::Retire() {
  if (retired_) return;
  retired_ = true;
  if (store_ != nullptr) store_->Disconnect();
  if (associated_) {
    associated_ = false;
    ITfDocumentMgr* replaced = nullptr;
    manager_->AssociateFocus(window_, previous_, &replaced);
    if (replaced != nullptr) replaced->Release();
  }
}
void TextService::Update(TextDocument document) { if (!retired_) store_->Update(std::move(document)); }
void TextService::Caret(const RECT& screen) { if (!retired_) store_->Caret(screen); }
void TextService::Geometry(TextGeometry geometry) { if (!retired_) store_->Geometry(std::move(geometry)); }
void TextService::Pump() { if (!retired_) store_->Pump(); }
void TextService::Focus(bool focused) { if (!retired_ && manager_ != nullptr) manager_->SetFocus(focused ? document_ : nullptr); }
bool TextService::Key(UINT message, WPARAM key, LPARAM flags) {
  if (retired_ || keys_ == nullptr) return false;
  BOOL eaten = FALSE;
  if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
    if (FAILED(keys_->TestKeyDown(key, flags, &eaten)) || !eaten || retired_) return false;
    return SUCCEEDED(keys_->KeyDown(key, flags, &eaten)) && eaten;
  }
  if (message == WM_KEYUP || message == WM_SYSKEYUP) {
    if (FAILED(keys_->TestKeyUp(key, flags, &eaten)) || !eaten || retired_) return false;
    return SUCCEEDED(keys_->KeyUp(key, flags, &eaten)) && eaten;
  }
  return false;
}
}  // namespace winapk::host::tsf
