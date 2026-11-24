// Adapted from the other runtime (https://example.invalid/other-runtime), compat/input/text_service.h,
// Copyright (C) 2026 its authors, SPDX-License-Identifier: Apache-2.0. Modified for Win.apk.
//
// tsf_text_service.h -- the window thread's TSF thread manager, document manager and context
// over one editor's TextStore. Attached when an editor that takes compositions gets the focus,
// retired when it loses it; a failed Attach leaves the window on IMM32 (win32_window.cpp).

#pragma once

#include <memory>

#include "tsf_text_store.h"

namespace winapk::host::tsf {

class TextService {
public:
    // On the window's thread, which must be an STA (CoInitializeEx done by the caller). Null,
    // with the failing HRESULT in *result, when TSF is not there or refuses.
    static std::shared_ptr<TextService> Attach(HWND window, TextDocument initial, TextStore::Emit emit,
                                               TextStore::Schedule schedule, HRESULT* result);
    ~TextService();
    // Restores the window's earlier document manager before another editor is attached. A COM
    // call still running keeps the disconnected context until it returns.
    void Retire();
    void Update(TextDocument document);
    void Caret(const RECT& screen);
    void Geometry(TextGeometry geometry);
    void Pump();
    // A key for the text services first (ITfKeystrokeMgr): true when one took it.
    bool Key(UINT message, WPARAM key, LPARAM flags);
    void Focus(bool focused);

private:
    TextService() = default;
    HWND window_ = nullptr;
    ITfThreadMgr* manager_ = nullptr;
    ITfDocumentMgr* document_ = nullptr;
    ITfDocumentMgr* previous_ = nullptr;
    ITfContext* context_ = nullptr;
    ITfKeystrokeMgr* keys_ = nullptr;
    TextStore* store_ = nullptr;
    TfClientId client_ = 0;
    bool activated_ = false, associated_ = false, retired_ = false;
};

}  // namespace winapk::host::tsf
