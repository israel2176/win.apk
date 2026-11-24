// Adapted from the other runtime (https://example.invalid/other-runtime), compat/input/touch_keyboard.cc,
// Copyright (C) 2026 its authors, SPDX-License-Identifier: Apache-2.0. Modified for Win.apk.
//
// touch_keyboard.cpp -- Windows' touch keyboard for a window (touch_keyboard.h): InputPane
// through IInputPaneInterop::GetForWindow, then TryShow or TryHide. The WinRT entry points are
// looked up in combase.dll at run time, so nothing more is linked. Called on the window's
// thread (win32_window.cpp). Win.apk's changes: the namespace, combase looked up once, and
// touch_keyboard_device.

#include <windows.h>

#include <cwchar>

#include "touch_keyboard.h"

namespace winapk::host {

namespace {

// IInspectable's slots, then the interface's own (windows.ui.viewmanagement.h).
struct IInspectableVtbl {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(void*, REFIID, void**);
    ULONG(STDMETHODCALLTYPE* AddRef)(void*);
    ULONG(STDMETHODCALLTYPE* Release)(void*);
    HRESULT(STDMETHODCALLTYPE* GetIids)(void*, ULONG*, IID**);
    HRESULT(STDMETHODCALLTYPE* GetRuntimeClassName)(void*, void**);
    HRESULT(STDMETHODCALLTYPE* GetTrustLevel)(void*, int*);
};
struct IInputPaneInteropVtbl {
    IInspectableVtbl base;
    HRESULT(STDMETHODCALLTYPE* GetForWindow)(void*, HWND, REFIID, void**);
};
struct IInputPane2Vtbl {
    IInspectableVtbl base;
    HRESULT(STDMETHODCALLTYPE* TryShow)(void*, unsigned char*);
    HRESULT(STDMETHODCALLTYPE* TryHide)(void*, unsigned char*);
};
template <typename V>
struct Object {
    const V* vtbl;
};

// 75CF2C57-9195-4931-8332-F0B409E916AF
const IID kIidInputPaneInterop = {0x75cf2c57, 0x9195, 0x4931, {0x83, 0x32, 0xf0, 0xb4, 0x09, 0xe9, 0x16, 0xaf}};
// 8A6B3F26-7090-4793-944C-C3F2CDE26276
const IID kIidInputPane2 = {0x8a6b3f26, 0x7090, 0x4793, {0x94, 0x4c, 0xc3, 0xf2, 0xcd, 0xe2, 0x62, 0x76}};

using RoInitializeFn = HRESULT(WINAPI*)(int);
using WindowsCreateStringFn = HRESULT(WINAPI*)(const wchar_t*, UINT32, void**);
using WindowsDeleteStringFn = HRESULT(WINAPI*)(void*);
using RoGetActivationFactoryFn = HRESULT(WINAPI*)(void*, REFIID, void**);

template <typename F>
F lookup(HMODULE module, const char* name) {
    return module ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(module, name))) : nullptr;
}

}  // namespace

bool touch_keyboard_device() {
    // A digitizer that takes fingers. Whether a hardware keyboard is attached, and the user's
    // "Show the touch keyboard" setting, are Windows' to weigh: TryShow declines then.
    return GetSystemMetrics(SM_MAXIMUMTOUCHES) > 0;
}

int32_t show_touch_keyboard(void* hwnd, bool show) {
    static const HMODULE combase = LoadLibraryW(L"combase.dll");
    if (combase == nullptr) return static_cast<int32_t>(HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND));
    static const auto ro_initialize = lookup<RoInitializeFn>(combase, "RoInitialize");
    static const auto create_string = lookup<WindowsCreateStringFn>(combase, "WindowsCreateString");
    static const auto delete_string = lookup<WindowsDeleteStringFn>(combase, "WindowsDeleteString");
    static const auto get_factory = lookup<RoGetActivationFactoryFn>(combase, "RoGetActivationFactory");
    if (ro_initialize == nullptr || create_string == nullptr || delete_string == nullptr || get_factory == nullptr) {
        return static_cast<int32_t>(E_NOTIMPL);
    }
    // The window's thread is single-threaded (it has a message loop); an earlier
    // initialization in another mode is kept.
    ro_initialize(0 /* RO_INIT_SINGLETHREADED */);
    static const wchar_t kClass[] = L"Windows.UI.ViewManagement.InputPane";
    void* name = nullptr;
    HRESULT hr = create_string(kClass, static_cast<UINT32>(std::wcslen(kClass)), &name);
    if (FAILED(hr)) return static_cast<int32_t>(hr);
    void* interop = nullptr;
    hr = get_factory(name, kIidInputPaneInterop, &interop);
    delete_string(name);
    if (FAILED(hr)) return static_cast<int32_t>(hr);
    auto* factory = static_cast<Object<IInputPaneInteropVtbl>*>(interop);
    void* pane = nullptr;
    hr = factory->vtbl->GetForWindow(factory, static_cast<HWND>(hwnd), kIidInputPane2, &pane);
    factory->vtbl->base.Release(factory);
    if (FAILED(hr)) return static_cast<int32_t>(hr);
    auto* input_pane = static_cast<Object<IInputPane2Vtbl>*>(pane);
    unsigned char done = 0;
    hr = show ? input_pane->vtbl->TryShow(input_pane, &done) : input_pane->vtbl->TryHide(input_pane, &done);
    input_pane->vtbl->base.Release(input_pane);
    if (FAILED(hr)) return static_cast<int32_t>(hr);
    return done ? 1 : 0;
}

}  // namespace winapk::host
