// input_pane.cpp -- see input_pane.h.

#include "input_pane.h"

#include <cstdint>

namespace winapk::host::input_pane {

namespace {

// The few WinRT declarations this needs (inspectable.h and windows.ui.viewmanagement.h are not
// in every MinGW): IInspectable's three methods, then each interface's own, in vtable order.
struct Inspectable : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRuntimeClassName(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(int*) = 0;
};
// Windows.UI.ViewManagement.IInputPaneInterop (inputpaneinterop.h).
struct InputPaneInterop : public Inspectable {
    virtual HRESULT STDMETHODCALLTYPE GetForWindow(HWND, REFIID, void**) = 0;
};
// Windows.UI.ViewManagement.IInputPane2.
struct InputPane2 : public Inspectable {
    virtual HRESULT STDMETHODCALLTYPE TryShow(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryHide(boolean*) = 0;
};
// {75CF2C57-9195-4931-8332-F0B409E916AF}
const IID kIID_InputPaneInterop = {0x75cf2c57, 0x9195, 0x4931, {0x83, 0x32, 0xf0, 0xb4, 0x09, 0xe9, 0x16, 0xaf}};
// {8A6B3F26-7090-4793-944C-C3F2CDE26276}
const IID kIID_InputPane2 = {0x8a6b3f26, 0x7090, 0x4793, {0x94, 0x4c, 0xc3, 0xf2, 0xcd, 0xe2, 0x62, 0x76}};

using HString = void*;
struct Combase {
    HRESULT(WINAPI* create_string)(const wchar_t*, UINT32, HString*) = nullptr;
    HRESULT(WINAPI* delete_string)(HString) = nullptr;
    HRESULT(WINAPI* activation_factory)(HString, REFIID, void**) = nullptr;
    bool ok() const { return create_string && delete_string && activation_factory; }
};

const Combase& combase() {
    static const Combase api = [] {
        Combase c;
        if (HMODULE dll = LoadLibraryW(L"combase.dll")) {
            c.create_string = reinterpret_cast<decltype(c.create_string)>(GetProcAddress(dll, "WindowsCreateString"));
            c.delete_string = reinterpret_cast<decltype(c.delete_string)>(GetProcAddress(dll, "WindowsDeleteString"));
            c.activation_factory =
                reinterpret_cast<decltype(c.activation_factory)>(GetProcAddress(dll, "RoGetActivationFactory"));
        }
        return c;
    }();
    return api;
}

// The window's input pane, or null with *why.
InputPane2* pane_for(HWND window, HRESULT* why) {
    const Combase& api = combase();
    if (!api.ok()) {
        *why = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    static const wchar_t kClass[] = L"Windows.UI.ViewManagement.InputPane";
    HString name = nullptr;
    HRESULT hr = api.create_string(kClass, static_cast<UINT32>(sizeof(kClass) / sizeof(kClass[0]) - 1), &name);
    InputPaneInterop* interop = nullptr;
    if (SUCCEEDED(hr)) {
        hr = api.activation_factory(name, kIID_InputPaneInterop, reinterpret_cast<void**>(&interop));
        api.delete_string(name);
    }
    InputPane2* pane = nullptr;
    if (SUCCEEDED(hr) && interop) hr = interop->GetForWindow(window, kIID_InputPane2, reinterpret_cast<void**>(&pane));
    if (interop) interop->Release();
    if (FAILED(hr) || !pane) {
        *why = FAILED(hr) ? hr : E_NOINTERFACE;
        return nullptr;
    }
    return pane;
}

}  // namespace

bool touch_screen() {
    constexpr int kDigitizer = 94;  // SM_DIGITIZER
    constexpr int kReady = 0x80, kTouch = 0x01 | 0x02, kPen = 0x04 | 0x08;  // NID_*
    const int d = GetSystemMetrics(kDigitizer);
    return (d & kReady) != 0 && (d & (kTouch | kPen)) != 0;
}

Answer show(HWND window, bool show, HRESULT* why) {
    HRESULT failure = S_OK;
    InputPane2* pane = pane_for(window, &failure);
    if (!pane) {
        if (why) *why = failure;
        return Answer::kUnavailable;
    }
    boolean done = 0;
    const HRESULT hr = show ? pane->TryShow(&done) : pane->TryHide(&done);
    pane->Release();
    if (FAILED(hr)) {
        if (why) *why = hr;
        return Answer::kUnavailable;
    }
    if (!done) return Answer::kDeclined;
    return show ? Answer::kShown : Answer::kHidden;
}

}  // namespace winapk::host::input_pane
