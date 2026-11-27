// native_printing.cpp -- com.macapk.loader.HostPrinting: Windows' printers for android.print.PrintManager
// (runtime/java/android/print/PrintSpooler.java drives the app's PrintDocumentAdapter as Android's print
// spooler does; this file is its print dialog and its print service).
//
//   nativeDefaults  the default printer's paper, resolution, colour and duplex, with the app's
//                   PrintAttributes applied where the printer has them: what the spooler lays the document
//                   out for before the user has chosen, as Android's print UI lays it out for the printer
//                   it selects first.
//   nativeChoose    Windows' own Print dialog (PrintDlgEx) over the app's window, on its own thread: the
//                   printer, its preferences (paper, orientation, colour, two-sided), copies and pages. The
//                   answer goes back through HostPrinting.onChosen with a setup handle (the printer's name
//                   and its DEVMODE) and what the choice means in Android's terms.
//   nativePrint     the PDF the app wrote, printed on that setup on its own thread: each page rendered by
//                   Windows.Data.Pdf (in Windows since 8.1; nothing to install) at the printer's resolution,
//                   up to 600 dpi, in bands, drawn into the printer's device context with GDI, so that the
//                   driver applies everything the user chose. Progress goes back through
//                   HostPrinting.onPrintState: started, completed, failed (with why) or cancelled.
//   nativeCancel    stops a print between bands; the document is aborted (AbortDoc), nothing half-printed is
//                   left in the queue.
//
// Where a page goes on the sheet and the order of pages and copies are print_layout.h's, which
// test/print_layout_test.cpp checks without a printer.

#include "host_natives.h"

#include <atomic>
#include <cstring>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <commdlg.h>
#include <inspectable.h>
#include <objidl.h>
#include <wincodec.h>
#include <winspool.h>

#include "print_layout.h"
#include "win32_window.h"

namespace winapk::host {

namespace {

// ---- the WinRT ABI used (Windows.Data.Pdf, Windows.Storage.Streams, Windows.Foundation) -----------------

struct SizeAbi { float width, height; };
struct RectAbi { float x, y, width, height; };
struct ColorAbi { BYTE a, r, g, b; };

constexpr GUID kIidAsyncInfo = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIidPdfDocumentStatics = {0x433a0b5f, 0xc007, 0x4788, {0x90, 0xf2, 0x08, 0x14, 0x3d, 0x92, 0x25, 0x99}};
constexpr GUID kIidPdfPageRenderOptions = {0x3c98056f, 0xb7cf, 0x4c29, {0x9a, 0x04, 0x52, 0xd9, 0x02, 0x67, 0xf4, 0x25}};
constexpr GUID kIidRandomAccessStream = {0x905a0fe1, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};

// Windows.Foundation.IAsyncInfo; AsyncStatus: Started 0, Completed 1, Canceled 2, Error 3.
struct IAsyncInfoAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
// IAsyncOperation<T> and IAsyncAction: the results after the completion handler's two methods. The
// pointers come typed from the call that made them, so their parameterized IIDs are not needed.
struct IAsyncOperationAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(IInspectable**) = 0;
};
struct IAsyncActionAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults() = 0;
};
struct IPdfPageRenderOptionsAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_SourceRect(RectAbi*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_SourceRect(RectAbi) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DestinationWidth(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DestinationWidth(UINT32) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DestinationHeight(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DestinationHeight(UINT32) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BackgroundColor(ColorAbi*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_BackgroundColor(ColorAbi) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsIgnoringHighContrast(boolean*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsIgnoringHighContrast(boolean) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BitmapEncoderId(GUID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_BitmapEncoderId(GUID) = 0;
};
struct IPdfPageAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RenderToStreamAsync(IUnknown* output, IAsyncActionAbi**) = 0;
    virtual HRESULT STDMETHODCALLTYPE RenderWithOptionsToStreamAsync(IUnknown* output, IPdfPageRenderOptionsAbi*,
                                                                     IAsyncActionAbi**) = 0;
    virtual HRESULT STDMETHODCALLTYPE PreparePageAsync(IAsyncActionAbi**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Index(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(SizeAbi*) = 0;
};
struct IPdfDocumentAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetPage(UINT32, IPdfPageAbi**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PageCount(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPasswordProtected(boolean*) = 0;
};
struct IPdfDocumentStaticsAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileAsync(IUnknown*, IAsyncOperationAbi**) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileWithPasswordAsync(IUnknown*, HSTRING, IAsyncOperationAbi**) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromStreamAsync(IUnknown* stream, IAsyncOperationAbi**) = 0;
};

template <class T>
struct Ref {
    T* p = nullptr;
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    ~Ref() {
        if (p) p->Release();
    }
    T* operator->() const { return p; }
    T** put() {
        if (p) p->Release();
        p = nullptr;
        return &p;
    }
    explicit operator bool() const { return p != nullptr; }
};

using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
using RoActivateInstanceFn = HRESULT(WINAPI*)(HSTRING, IInspectable**);
using WindowsCreateStringFn = HRESULT(WINAPI*)(const wchar_t*, UINT32, HSTRING*);
using WindowsDeleteStringFn = HRESULT(WINAPI*)(HSTRING);
using CreateRandomAccessStreamOnFileFn = HRESULT(WINAPI*)(PCWSTR, DWORD, REFIID, void**);
using CreateStreamOverRandomAccessStreamFn = HRESULT(WINAPI*)(IUnknown*, REFIID, void**);

template <class F>
F proc(HMODULE m, const char* name) {
    return m ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}

struct WinRt {
    RoGetActivationFactoryFn factory = nullptr;
    RoActivateInstanceFn activate = nullptr;
    WindowsCreateStringFn create_string = nullptr;
    WindowsDeleteStringFn delete_string = nullptr;
    CreateRandomAccessStreamOnFileFn stream_on_file = nullptr;
    CreateStreamOverRandomAccessStreamFn istream_over = nullptr;
    WinRt() {
        HMODULE combase = LoadLibraryW(L"combase.dll");
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        factory = proc<RoGetActivationFactoryFn>(combase, "RoGetActivationFactory");
        activate = proc<RoActivateInstanceFn>(combase, "RoActivateInstance");
        create_string = proc<WindowsCreateStringFn>(combase, "WindowsCreateString");
        delete_string = proc<WindowsDeleteStringFn>(combase, "WindowsDeleteString");
        stream_on_file = proc<CreateRandomAccessStreamOnFileFn>(shcore, "CreateRandomAccessStreamOnFile");
        istream_over = proc<CreateStreamOverRandomAccessStreamFn>(shcore, "CreateStreamOverRandomAccessStream");
    }
    bool ok() const {
        return factory && activate && create_string && delete_string && stream_on_file && istream_over;
    }
    HRESULT instance(const wchar_t* cls, REFIID iid, void** out) const {
        HSTRING h = nullptr;
        HRESULT hr = create_string(cls, static_cast<UINT32>(wcslen(cls)), &h);
        if (FAILED(hr)) return hr;
        Ref<IInspectable> obj;
        hr = activate(h, obj.put());
        delete_string(h);
        return SUCCEEDED(hr) ? obj->QueryInterface(iid, out) : hr;
    }
    HRESULT statics(const wchar_t* cls, REFIID iid, void** out) const {
        HSTRING h = nullptr;
        HRESULT hr = create_string(cls, static_cast<UINT32>(wcslen(cls)), &h);
        if (FAILED(hr)) return hr;
        hr = factory(h, iid, out);
        delete_string(h);
        return hr;
    }
};

const WinRt& winrt() {
    static const WinRt w;
    return w;
}

// Waits for an asynchronous operation, pumping this (single-threaded apartment) thread's messages, as a
// printer driver's own window on this thread needs; false when it failed, was cancelled, or `cancel`
// was set (the operation is then cancelled).
bool wait_async(IInspectable* op, const std::atomic<bool>* cancel, HRESULT* error) {
    Ref<IAsyncInfoAbi> info;
    if (FAILED(op->QueryInterface(kIidAsyncInfo, reinterpret_cast<void**>(info.put())))) return false;
    const ULONGLONG deadline = GetTickCount64() + 120000;
    for (;;) {
        int status = 0;
        if (FAILED(info->get_Status(&status))) return false;
        if (status == 1) return true;
        if (status != 0) {
            if (error) info->get_ErrorCode(error);
            return false;
        }
        if ((cancel && cancel->load()) || GetTickCount64() > deadline) {
            info->Cancel();
            return false;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

// ---- JNI helpers ----------------------------------------------------------------------------------------

JavaVM* g_vm = nullptr;
jclass g_cls = nullptr;  // HostPrinting, a global reference

std::wstring wide(JNIEnv* env, jstring s) {
    if (!s) return {};
    const jchar* c = env->GetStringChars(s, nullptr);
    std::wstring w(reinterpret_cast<const wchar_t*>(c), static_cast<size_t>(env->GetStringLength(s)));
    env->ReleaseStringChars(s, c);
    return w;
}

jstring jstr(JNIEnv* env, const std::wstring& s) {
    return env->NewString(reinterpret_cast<const jchar*>(s.data()), static_cast<jsize>(s.size()));
}

bool remember_class(JNIEnv* env, jclass cls) {
    if (!g_vm && env->GetJavaVM(&g_vm) != JNI_OK) return false;
    if (!g_cls) g_cls = static_cast<jclass>(env->NewGlobalRef(cls));
    return g_cls != nullptr;
}

// Runs `f` with this host thread attached to the JVM.
template <class F>
void with_java(F f) {
    JNIEnv* env = nullptr;
    if (!g_vm || g_vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), nullptr) != JNI_OK) return;
    f(env);
    if (env->ExceptionCheck()) env->ExceptionClear();
    g_vm->DetachCurrentThread();
}

// ---- a printer and its settings -------------------------------------------------------------------------

// What the dialog chose (or the default printer): the device and its DEVMODE, which the printing thread
// opens its own device context with, and the copies and collation the dialog left to the program.
struct Setup {
    std::wstring device, port;
    std::vector<BYTE> devmode;
    int copies = 1;
    bool collate = true;
    const DEVMODEW* dm() const { return devmode.empty() ? nullptr : reinterpret_cast<const DEVMODEW*>(devmode.data()); }
};

std::mutex g_lock;
std::map<jlong, Setup*> g_setups;           // handle -> setup, under g_lock
std::map<jint, std::atomic<bool>*> g_jobs;  // print id -> its cancel flag, under g_lock
jlong g_next_setup = 1;

// numbers[]: paper width, height, left, top, right, bottom margins (mils, oriented); dpi x, y; colour as
// DEVMODE has it (0 not said, 1 monochrome, 2 colour); a colour device (0/1); duplex as DEVMODE has it (0
// not said, 1 simplex, 2 long edge, 3 short edge); copies; collate (0/1). HostPrinting.java reads them.
constexpr int kNumbers = 13;

bool describe(const Setup& s, std::wstring* paper, int numbers[kNumbers]) {
    HDC dc = CreateDCW(L"WINSPOOL", s.device.c_str(), nullptr, s.dm());
    if (!dc) return false;
    PrinterGeometry g;
    g.dpi_x = GetDeviceCaps(dc, LOGPIXELSX);
    g.dpi_y = GetDeviceCaps(dc, LOGPIXELSY);
    g.paper_w = GetDeviceCaps(dc, PHYSICALWIDTH);
    g.paper_h = GetDeviceCaps(dc, PHYSICALHEIGHT);
    g.offset_x = GetDeviceCaps(dc, PHYSICALOFFSETX);
    g.offset_y = GetDeviceCaps(dc, PHYSICALOFFSETY);
    g.printable_w = GetDeviceCaps(dc, HORZRES);
    g.printable_h = GetDeviceCaps(dc, VERTRES);
    DeleteDC(dc);
    const PaperMils m = paper_mils(g);
    if (m.width <= 0 || m.height <= 0) return false;
    const DEVMODEW* dm = s.dm();
    numbers[0] = m.width;
    numbers[1] = m.height;
    numbers[2] = m.left;
    numbers[3] = m.top;
    numbers[4] = m.right;
    numbers[5] = m.bottom;
    numbers[6] = g.dpi_x;
    numbers[7] = g.dpi_y;
    numbers[8] = dm && (dm->dmFields & DM_COLOR) ? dm->dmColor : 0;
    numbers[9] = DeviceCapabilitiesW(s.device.c_str(), s.port.empty() ? nullptr : s.port.c_str(), DC_COLORDEVICE,
                                     nullptr, dm) == 1
                     ? 1
                     : 0;
    numbers[10] = dm && (dm->dmFields & DM_DUPLEX) ? dm->dmDuplex : 0;
    numbers[11] = s.copies;
    numbers[12] = s.collate ? 1 : 0;
    paper->clear();
    if (dm && (dm->dmFields & DM_FORMNAME)) paper->assign(dm->dmFormName);
    return true;
}

bool default_setup(Setup* s) {
    DWORD n = 0;
    GetDefaultPrinterW(nullptr, &n);
    if (n == 0) return false;
    std::wstring name(n, L'\0');
    if (!GetDefaultPrinterW(name.data(), &n)) return false;
    name.resize(wcslen(name.c_str()));
    HANDLE printer = nullptr;
    if (!OpenPrinterW(name.data(), &printer, nullptr)) return false;
    const LONG size = DocumentPropertiesW(nullptr, printer, name.data(), nullptr, nullptr, 0);
    if (size > 0) {
        s->devmode.resize(static_cast<size_t>(size));
        if (DocumentPropertiesW(nullptr, printer, name.data(), reinterpret_cast<DEVMODEW*>(s->devmode.data()), nullptr,
                                DM_OUT_BUFFER) != IDOK) {
            s->devmode.clear();
        }
    }
    ClosePrinter(printer);
    s->device = name;
    return true;
}

// The app's PrintAttributes as Windows' settings (HostPrinting.java, hints): Windows' paper code
// (DMPAPER_*), orientation (DMORIENT_*), colour (DMCOLOR_*), duplex (DMDUP_*); 0 for what the app did not
// say. Android's print UI keeps the app's paper, colour and two-sided when the printer it selects has
// them; so does this, asking the driver what it has.
struct Hints {
    int paper = 0, orientation = 0, color = 0, duplex = 0;
};

Hints hints_of(JNIEnv* env, jintArray a) {
    Hints h;
    if (!a || env->GetArrayLength(a) < 4) return h;
    jint v[4] = {};
    env->GetIntArrayRegion(a, 0, 4, v);
    h.paper = v[0];
    h.orientation = v[1];
    h.color = v[2];
    h.duplex = v[3];
    return h;
}

void apply_hints(Setup* s, const Hints& h) {
    if (s->devmode.size() < sizeof(DEVMODEW)) return;
    auto* dm = reinterpret_cast<DEVMODEW*>(s->devmode.data());
    const wchar_t* port = s->port.empty() ? nullptr : s->port.c_str();
    if (h.paper > 0) {
        const DWORD n = DeviceCapabilitiesW(s->device.c_str(), port, DC_PAPERS, nullptr, dm);
        if (n != static_cast<DWORD>(-1) && n > 0 && n < 4096) {
            std::vector<WORD> papers(n);
            DeviceCapabilitiesW(s->device.c_str(), port, DC_PAPERS, reinterpret_cast<wchar_t*>(papers.data()), dm);
            for (WORD p : papers) {
                if (p == h.paper) {
                    dm->dmFields = (dm->dmFields & ~static_cast<DWORD>(DM_PAPERLENGTH | DM_PAPERWIDTH | DM_FORMNAME)) |
                                   DM_PAPERSIZE;
                    dm->dmPaperSize = static_cast<short>(h.paper);
                    break;
                }
            }
        }
    }
    if (h.orientation == DMORIENT_PORTRAIT || h.orientation == DMORIENT_LANDSCAPE) {
        dm->dmFields |= DM_ORIENTATION;
        dm->dmOrientation = static_cast<short>(h.orientation);
    }
    if (h.color == DMCOLOR_MONOCHROME ||
        (h.color == DMCOLOR_COLOR && DeviceCapabilitiesW(s->device.c_str(), port, DC_COLORDEVICE, nullptr, dm) == 1)) {
        dm->dmFields |= DM_COLOR;
        dm->dmColor = static_cast<short>(h.color);
    }
    if (h.duplex >= DMDUP_SIMPLEX && h.duplex <= DMDUP_HORIZONTAL &&
        (h.duplex == DMDUP_SIMPLEX || DeviceCapabilitiesW(s->device.c_str(), port, DC_DUPLEX, nullptr, dm) == 1)) {
        dm->dmFields |= DM_DUPLEX;
        dm->dmDuplex = static_cast<short>(h.duplex);
    }
    // The driver settles the changes into a DEVMODE it accepts (DM_IN_BUFFER | DM_OUT_BUFFER).
    HANDLE printer = nullptr;
    if (OpenPrinterW(s->device.data(), &printer, nullptr)) {
        std::vector<BYTE> out(s->devmode.size());
        if (DocumentPropertiesW(nullptr, printer, s->device.data(), reinterpret_cast<DEVMODEW*>(out.data()), dm,
                                DM_IN_BUFFER | DM_OUT_BUFFER) == IDOK) {
            s->devmode.swap(out);
        }
        ClosePrinter(printer);
    }
}

jboolean JNICALL Printing_defaults(JNIEnv* env, jclass, jintArray hints, jobjectArray names, jintArray numbers) {
    Setup s;
    std::wstring paper;
    int n[kNumbers] = {};
    if (!names || !numbers || env->GetArrayLength(names) < 2 || env->GetArrayLength(numbers) < kNumbers) {
        return JNI_FALSE;
    }
    if (!default_setup(&s)) return JNI_FALSE;
    apply_hints(&s, hints_of(env, hints));
    if (!describe(s, &paper, n)) return JNI_FALSE;
    env->SetObjectArrayElement(names, 0, jstr(env, s.device));
    env->SetObjectArrayElement(names, 1, jstr(env, paper));
    env->SetIntArrayRegion(numbers, 0, kNumbers, reinterpret_cast<const jint*>(n));
    return JNI_TRUE;
}

// ---- the print dialog -----------------------------------------------------------------------------------

constexpr int kChosen = 0, kCancelled = 1, kFailed = 2;

void choose(int page_count, Hints hints, jint id) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HWND owner = static_cast<HWND>(window_handle());
    if (owner) owner = GetAncestor(owner, GA_ROOT);

    // The dialog opens on the default printer with the app's settings applied (its DEVMODE names it).
    HGLOBAL initial = nullptr;
    Setup first;
    if (default_setup(&first) && !first.devmode.empty()) {
        apply_hints(&first, hints);
        if ((initial = GlobalAlloc(GMEM_MOVEABLE, first.devmode.size()))) {
            if (void* p = GlobalLock(initial)) {
                std::memcpy(p, first.devmode.data(), first.devmode.size());
                GlobalUnlock(initial);
            }
        }
    }

    PRINTPAGERANGE ranges[32] = {};
    PRINTDLGEXW pd = {};
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner = owner ? owner : GetDesktopWindow();
    // Copies and collation are the program's (nCopies, PD_COLLATE): the Copies box works on every driver.
    pd.Flags = PD_NOSELECTION | PD_NOCURRENTPAGE;
    pd.nPageRanges = 0;
    pd.nMaxPageRanges = 32;
    pd.lpPageRanges = ranges;
    pd.nMinPage = 1;
    // A document whose page count the app did not give (PAGE_COUNT_UNKNOWN) takes any page number.
    pd.nMaxPage = page_count > 0 ? static_cast<DWORD>(page_count) : 9999;
    pd.nCopies = 1;
    pd.nStartPage = START_PAGE_GENERAL;
    pd.hDevMode = initial;
    const HRESULT hr = PrintDlgExW(&pd);

    int result = kCancelled;
    std::wstring why;
    Setup* setup = nullptr;
    if (FAILED(hr)) {
        result = kFailed;
        const DWORD code = CommDlgExtendedError();
        why = code == PDERR_NODEFAULTPRN || code == PDERR_NODEVICES ? L"No printer is installed"
                                                                     : L"The print dialog could not open";
    } else if (pd.dwResultAction == PD_RESULT_PRINT && pd.hDevNames) {
        setup = new Setup;
        if (auto* names = static_cast<DEVNAMES*>(GlobalLock(pd.hDevNames))) {
            const wchar_t* base = reinterpret_cast<const wchar_t*>(names);
            setup->device = base + names->wDeviceOffset;
            setup->port = base + names->wOutputOffset;
            GlobalUnlock(pd.hDevNames);
        }
        if (pd.hDevMode) {
            if (auto* dm = static_cast<DEVMODEW*>(GlobalLock(pd.hDevMode))) {
                const BYTE* b = reinterpret_cast<const BYTE*>(dm);
                setup->devmode.assign(b, b + dm->dmSize + dm->dmDriverExtra);
                GlobalUnlock(pd.hDevMode);
            }
        }
        setup->copies = static_cast<int>(pd.nCopies ? pd.nCopies : 1);
        setup->collate = (pd.Flags & PD_COLLATE) != 0;
        result = kChosen;
    }
    if (pd.hDevMode) GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
    if (pd.hDC) DeleteDC(pd.hDC);

    std::wstring paper;
    int n[kNumbers] = {};
    if (setup && !describe(*setup, &paper, n)) {
        delete setup;
        setup = nullptr;
        result = kFailed;
        why = L"The printer could not be opened";
    }
    jlong handle = 0;
    if (setup) {
        std::lock_guard<std::mutex> l(g_lock);
        handle = g_next_setup++;
        g_setups[handle] = setup;
    }
    // Page ranges as the dialog gives them: 1-based, inclusive; none (all pages) when PD_PAGENUMS is not set.
    std::vector<jint> pages;
    if (result == kChosen && (pd.Flags & PD_PAGENUMS)) {
        for (DWORD i = 0; i < pd.nPageRanges && i < 32; ++i) {
            pages.push_back(static_cast<jint>(ranges[i].nFromPage));
            pages.push_back(static_cast<jint>(ranges[i].nToPage));
        }
    }
    CoUninitialize();

    with_java([&](JNIEnv* env) {
        jobjectArray names = nullptr;
        jintArray numbers = nullptr, range_array = nullptr;
        if (result == kChosen) {
            names = env->NewObjectArray(2, env->FindClass("java/lang/String"), nullptr);
            env->SetObjectArrayElement(names, 0, jstr(env, setup->device));
            env->SetObjectArrayElement(names, 1, jstr(env, paper));
            numbers = env->NewIntArray(kNumbers);
            env->SetIntArrayRegion(numbers, 0, kNumbers, reinterpret_cast<const jint*>(n));
            range_array = env->NewIntArray(static_cast<jsize>(pages.size()));
            if (!pages.empty()) env->SetIntArrayRegion(range_array, 0, static_cast<jsize>(pages.size()), pages.data());
        } else if (result == kFailed) {
            names = env->NewObjectArray(1, env->FindClass("java/lang/String"), nullptr);
            env->SetObjectArrayElement(names, 0, jstr(env, why));
        }
        jmethodID m = env->GetStaticMethodID(g_cls, "onChosen", "(IIJ[Ljava/lang/String;[I[I)V");
        if (m) env->CallStaticVoidMethod(g_cls, m, id, result, handle, names, numbers, range_array);
    });
}

jboolean JNICALL Printing_choose(JNIEnv* env, jclass cls, jint page_count, jintArray hints, jint id) {
    if (!remember_class(env, cls)) return JNI_FALSE;
    std::thread(choose, static_cast<int>(page_count), hints_of(env, hints), id).detach();
    return JNI_TRUE;
}

void JNICALL Printing_release(JNIEnv*, jclass, jlong handle) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_setups.find(handle);
    if (it == g_setups.end()) return;
    delete it->second;
    g_setups.erase(it);
}

// ---- printing -------------------------------------------------------------------------------------------

constexpr int kStarted = 1, kCompleted = 2, kPrintFailed = 3, kPrintCancelled = 4;

void report(jint id, int state, const std::wstring& status) {
    with_java([&](JNIEnv* env) {
        jstring s = status.empty() ? nullptr : jstr(env, status);
        jmethodID m = env->GetStaticMethodID(g_cls, "onPrintState", "(IILjava/lang/String;)V");
        if (m) env->CallStaticVoidMethod(g_cls, m, id, state, s);
    });
}

std::wstring hex(const wchar_t* what, HRESULT hr) {
    wchar_t buf[160];
    std::swprintf(buf, 160, L"%ls (0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

struct Pdf {
    Ref<IPdfDocumentAbi> doc;
    UINT32 pages = 0;
};

HRESULT load_pdf(const std::wstring& path, Pdf* pdf, const std::atomic<bool>* cancel) {
    const WinRt& rt = winrt();
    Ref<IUnknown> file;
    HRESULT hr = rt.stream_on_file(path.c_str(), 0 /* FileAccessMode.Read */, kIidRandomAccessStream,
                                   reinterpret_cast<void**>(file.put()));
    if (FAILED(hr)) return hr;
    Ref<IPdfDocumentStaticsAbi> statics;
    hr = rt.statics(L"Windows.Data.Pdf.PdfDocument", kIidPdfDocumentStatics, reinterpret_cast<void**>(statics.put()));
    if (FAILED(hr)) return hr;
    Ref<IAsyncOperationAbi> op;
    hr = statics->LoadFromStreamAsync(file.p, op.put());
    if (FAILED(hr)) return hr;
    HRESULT err = E_FAIL;
    if (!wait_async(op.p, cancel, &err)) return FAILED(err) ? err : E_ABORT;
    Ref<IInspectable> result;
    hr = op->GetResults(result.put());
    if (FAILED(hr) || !result) return FAILED(hr) ? hr : E_FAIL;
    // The result is the PdfDocument's default interface, IPdfDocument.
    pdf->doc.p = reinterpret_cast<IPdfDocumentAbi*>(result.p);
    result.p = nullptr;
    return pdf->doc->get_PageCount(&pdf->pages);
}

// Renders one band of `page` and draws it into `dc`. `pixels` is reused between bands.
HRESULT draw_band(HDC dc, IPdfPageAbi* page, double page_w, const PagePlacement& p, const PageBand& b,
                  IWICImagingFactory* wic, std::vector<BYTE>* pixels, const std::atomic<bool>* cancel) {
    const WinRt& rt = winrt();
    Ref<IPdfPageRenderOptionsAbi> opts;
    HRESULT hr = rt.instance(L"Windows.Data.Pdf.PdfPageRenderOptions", kIidPdfPageRenderOptions,
                             reinterpret_cast<void**>(opts.put()));
    if (FAILED(hr)) return hr;
    opts->put_SourceRect(RectAbi{0.0f, static_cast<float>(b.src_y), static_cast<float>(page_w),
                                 static_cast<float>(b.src_h)});
    opts->put_DestinationWidth(static_cast<UINT32>(p.render_w));
    opts->put_DestinationHeight(static_cast<UINT32>(b.rows));
    opts->put_BackgroundColor(ColorAbi{255, 255, 255, 255});
    // Paper is printed in the document's colours, never Windows' high-contrast theme.
    opts->put_IsIgnoringHighContrast(TRUE);
    Ref<IUnknown> stream;
    hr = rt.instance(L"Windows.Storage.Streams.InMemoryRandomAccessStream", kIidRandomAccessStream,
                     reinterpret_cast<void**>(stream.put()));
    if (FAILED(hr)) return hr;
    Ref<IAsyncActionAbi> action;
    hr = page->RenderWithOptionsToStreamAsync(stream.p, opts.p, action.put());
    if (FAILED(hr)) return hr;
    HRESULT err = E_FAIL;
    if (!wait_async(action.p, cancel, &err)) return FAILED(err) ? err : E_ABORT;
    hr = action->GetResults();
    if (FAILED(hr)) return hr;

    Ref<IStream> png;
    hr = rt.istream_over(stream.p, IID_IStream, reinterpret_cast<void**>(png.put()));
    if (FAILED(hr)) return hr;
    LARGE_INTEGER zero = {};
    png->Seek(zero, STREAM_SEEK_SET, nullptr);
    Ref<IWICBitmapDecoder> decoder;
    hr = wic->CreateDecoderFromStream(png.p, nullptr, WICDecodeMetadataCacheOnDemand, decoder.put());
    if (FAILED(hr)) return hr;
    Ref<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.put());
    if (FAILED(hr)) return hr;
    Ref<IWICFormatConverter> bgr;
    hr = wic->CreateFormatConverter(bgr.put());
    if (FAILED(hr)) return hr;
    hr = bgr->Initialize(frame.p, GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                         WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;
    UINT w = 0, h = 0;
    bgr->GetSize(&w, &h);
    if (w == 0 || h == 0) return E_FAIL;
    const UINT stride = w * 4;
    pixels->resize(static_cast<size_t>(stride) * h);
    hr = bgr->CopyPixels(nullptr, stride, static_cast<UINT>(pixels->size()), pixels->data());
    if (FAILED(hr)) return hr;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = static_cast<LONG>(w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(h);  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    const int x = static_cast<int>(std::lround(p.x));
    const int dw = static_cast<int>(std::lround(p.w));
    if (StretchDIBits(dc, x, b.dst_y, dw, b.dst_h, 0, 0, static_cast<int>(w), static_cast<int>(h), pixels->data(), &bi,
                      DIB_RGB_COLORS, SRCCOPY) == 0) {
        return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_PRINTER_DRIVER_BLOCKED);
    }
    return S_OK;
}

struct PrintRequest {
    Setup setup;
    std::wstring path, job;
    std::vector<int> pages;
    jint id = 0;
    std::atomic<bool>* cancel = nullptr;
};

void print(PrintRequest r) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int state = kCompleted;
    std::wstring status;
    HDC dc = nullptr;
    bool doc_open = false;
    Pdf pdf;
    Ref<IWICImagingFactory> wic;
    HRESULT hr = S_OK;
    if (!winrt().ok()) {
        state = kPrintFailed;
        status = L"Windows cannot render PDF documents here (Windows.Data.Pdf is missing)";
    } else if (FAILED(hr = load_pdf(r.path, &pdf, r.cancel))) {
        state = r.cancel->load() ? kPrintCancelled : kPrintFailed;
        status = hex(L"Windows could not read the document", hr);
    } else if (FAILED(hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                            IID_IWICImagingFactory, reinterpret_cast<void**>(wic.put())))) {
        state = kPrintFailed;
        status = hex(L"Windows' imaging component is unavailable", hr);
    } else if (!(dc = CreateDCW(L"WINSPOOL", r.setup.device.c_str(), nullptr, r.setup.dm()))) {
        state = kPrintFailed;
        status = L"The printer \"" + r.setup.device + L"\" could not be opened";
    } else {
        PrinterGeometry g;
        g.dpi_x = GetDeviceCaps(dc, LOGPIXELSX);
        g.dpi_y = GetDeviceCaps(dc, LOGPIXELSY);
        g.paper_w = GetDeviceCaps(dc, PHYSICALWIDTH);
        g.paper_h = GetDeviceCaps(dc, PHYSICALHEIGHT);
        g.offset_x = GetDeviceCaps(dc, PHYSICALOFFSETX);
        g.offset_y = GetDeviceCaps(dc, PHYSICALOFFSETY);
        g.printable_w = GetDeviceCaps(dc, HORZRES);
        g.printable_h = GetDeviceCaps(dc, VERTRES);

        DOCINFOW di = {};
        di.cbSize = sizeof(di);
        di.lpszDocName = r.job.c_str();
        // A printer that writes a file (Microsoft Print to PDF, XPS) asks where, here, inside StartDoc.
        if (StartDocW(dc, &di) <= 0) {
            const DWORD e = GetLastError();
            state = e == ERROR_CANCELLED || e == ERROR_PRINT_CANCELLED ? kPrintCancelled : kPrintFailed;
            status = state == kPrintCancelled ? L"" : hex(L"The printer did not accept the document",
                                                          HRESULT_FROM_WIN32(e));
        } else {
            doc_open = true;
            report(r.id, kStarted, L"");
            const std::vector<int> seq =
                print_sequence(r.pages, static_cast<int>(pdf.pages), r.setup.copies, r.setup.collate);
            std::vector<BYTE> pixels;
            for (size_t i = 0; i < seq.size() && state == kCompleted; ++i) {
                Ref<IPdfPageAbi> page;
                SizeAbi size = {};
                if (FAILED(hr = pdf.doc->GetPage(static_cast<UINT32>(seq[i]), page.put())) ||
                    FAILED(hr = page->get_Size(&size))) {
                    state = kPrintFailed;
                    status = hex(L"Windows could not read a page of the document", hr);
                    break;
                }
                const PagePlacement place = place_page(size.width, size.height, g);
                if (StartPage(dc) <= 0) {
                    state = kPrintFailed;
                    status = hex(L"The printer did not start a page", HRESULT_FROM_WIN32(GetLastError()));
                    break;
                }
                SetStretchBltMode(dc, HALFTONE);
                SetBrushOrgEx(dc, 0, 0, nullptr);
                for (const PageBand& b : page_bands(place, size.height)) {
                    if (r.cancel->load()) {
                        state = kPrintCancelled;
                        break;
                    }
                    if (FAILED(hr = draw_band(dc, page.p, size.width, place, b, wic.p, &pixels, r.cancel))) {
                        state = r.cancel->load() ? kPrintCancelled : kPrintFailed;
                        status = state == kPrintCancelled ? L"" : hex(L"Windows could not render a page", hr);
                        break;
                    }
                }
                if (state != kCompleted) break;
                if (EndPage(dc) <= 0) {
                    state = kPrintFailed;
                    status = hex(L"The printer did not take a page", HRESULT_FROM_WIN32(GetLastError()));
                }
            }
            if (state == kCompleted && seq.empty()) {
                state = kPrintFailed;
                status = L"The document has none of the pages chosen";
            }
        }
    }
    if (doc_open) {
        if (state == kCompleted) {
            if (EndDoc(dc) <= 0) {
                state = kPrintFailed;
                status = hex(L"The printer did not finish the document", HRESULT_FROM_WIN32(GetLastError()));
            }
        } else {
            AbortDoc(dc);
        }
    }
    if (dc) DeleteDC(dc);
    pdf.doc.put();
    wic.put();
    CoUninitialize();
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_jobs.erase(r.id);
    }
    delete r.cancel;
    std::fprintf(stderr, "[win.apk] print \"%ls\" on \"%ls\": %s%s%ls\n", r.job.c_str(), r.setup.device.c_str(),
                 state == kCompleted ? "completed" : state == kPrintCancelled ? "cancelled" : "failed",
                 status.empty() ? "" : ": ", status.c_str());
    std::fflush(stderr);
    report(r.id, state, status);
}

jboolean JNICALL Printing_print(JNIEnv* env, jclass cls, jlong handle, jstring path, jstring job, jintArray pages,
                                jint id) {
    if (!remember_class(env, cls)) return JNI_FALSE;
    PrintRequest r;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_setups.find(handle);
        if (it == g_setups.end()) return JNI_FALSE;
        r.setup = *it->second;
        r.cancel = new std::atomic<bool>(false);
        g_jobs[id] = r.cancel;
    }
    r.path = wide(env, path);
    r.job = wide(env, job);
    r.id = id;
    const jsize n = pages ? env->GetArrayLength(pages) : 0;
    if (n > 0) {
        std::vector<jint> p(static_cast<size_t>(n));
        env->GetIntArrayRegion(pages, 0, n, p.data());
        r.pages.assign(p.begin(), p.end());
    }
    std::thread(print, std::move(r)).detach();
    return JNI_TRUE;
}

void JNICALL Printing_cancel(JNIEnv*, jclass, jint id) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_jobs.find(id);
    if (it != g_jobs.end()) it->second->store(true);
}

constexpr const char* kCls = "com.macapk.loader.HostPrinting";

const HostMethod kMethods[] = {
    WINAPK_HOST_NATIVE(kCls, "nativeDefaults", "([I[Ljava/lang/String;[I)Z", Printing_defaults),
    WINAPK_HOST_NATIVE(kCls, "nativeChoose", "(I[II)Z", Printing_choose),
    WINAPK_HOST_NATIVE(kCls, "nativePrint", "(JLjava/lang/String;Ljava/lang/String;[II)Z", Printing_print),
    WINAPK_HOST_NATIVE(kCls, "nativeCancel", "(I)V", Printing_cancel),
    WINAPK_HOST_NATIVE(kCls, "nativeRelease", "(J)V", Printing_release),
};

}  // namespace

HostMethods native_printing_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

}  // namespace winapk::host
