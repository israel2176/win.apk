// apk_thumbnail.cpp -- winapk-shell.dll, the thumbnail handler of Win.apk's app files (WinApk.Package:
// .apk, .apkm, .xapk, .apks). Explorer's large and medium icons, and its preview pane's picture, show
// each file's own app icon, read from the file without installing it; the small icons and the details
// view keep the file type's icon (Win.apk.exe's).
//
// Adapted from the other runtime (https://example.invalid/other-runtime), compat/shell/other_shellext.cc,
// Copyright (C) 2026 its authors, SPDX-License-Identifier: Apache-2.0. Modified for Win.apk:
// the other runtime's is an icon handler (IExtractIconW) that runs `other-launch apk-icon` on the file, starting
// its Android runtime, and draws in the background; this is a thumbnail handler (IThumbnailProvider)
// that parses the file itself (apk_icon.cpp) and decodes the picture with WIC. Kept from the other runtime: the
// COM server's shape, its IIDs spelled out, and the DLL pinned once loaded (DllMain).
//
// Safety: no code of the APK runs and no process starts. The shell runs a thumbnail handler that does
// not set DisableProcessIsolation in a separate, low-rights surrogate (dllhost.exe), never in
// Explorer, and hands it the file as a read-only stream (IInitializeWithStream): the parsing is
// bounded (apk_icon.cpp), and a damaged file only means no thumbnail. The shell caches what comes
// back, per file, until the file changes.
//
// When the file's icon is a vector (or an adaptive icon with a vector layer), which only Android's own
// code draws, the icon Win.apk's install rendered for the same package is used when the app is
// installed (%LOCALAPPDATA%\Win.apk\library\<name>.package's first line, apps\<name>\icon.png); else
// no thumbnail, and Explorer shows the type's icon.
#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <wincodec.h>

#include <cmath>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "apk_icon.h"

using winapk::shell::ApkIcon;
using winapk::shell::Layer;

namespace {

// {B5A3C1E2-7D4F-4C8B-9E61-2F0A8D3C5B17}: WinApk.Package's thumbnail handler. Also in
// windows/launcher/launcher_main.cpp (register_file_types) and windows/package/winapk.iss.
const CLSID kClsidThumbnail = {0xb5a3c1e2, 0x7d4f, 0x4c8b, {0x9e, 0x61, 0x2f, 0x0a, 0x8d, 0x3c, 0x5b, 0x17}};
// The interfaces' IIDs, spelled out: not every one is in MinGW's libuuid (as in the other runtime's).
const IID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID kIidClassFactory = {0x00000001, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID kIidInitializeWithStream = {0xb824b49d, 0x22ac, 0x4161, {0xac, 0x8a, 0x99, 0x16, 0xe8, 0xfa, 0x3f, 0x7f}};
const IID kIidThumbnailProvider = {0xe357fccd, 0xa995, 0x4576, {0xb0, 0x1f, 0x23, 0x46, 0x30, 0x15, 0x4e, 0x96}};
const CLSID kClsidWicFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
const IID kIidWicFactory = {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
const GUID kPixelPbgra = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10}};

volatile LONG g_objects = 0;

// The stream the shell hands over, as apk_icon's random access. Reads are serialized by the handler's
// one thread (a thumbnail handler is apartment threaded).
class StreamSource final : public winapk::shell::Source {
public:
    explicit StreamSource(IStream* s) : s_(s) {
        STATSTG st{};
        if (SUCCEEDED(s_->Stat(&st, STATFLAG_NONAME))) size_ = st.cbSize.QuadPart;
    }
    uint64_t size() const override { return size_; }
    bool read(uint64_t off, void* dst, size_t n) const override {
        if (off > size_ || n > size_ - off) return false;
        LARGE_INTEGER at;
        at.QuadPart = static_cast<LONGLONG>(off);
        if (FAILED(s_->Seek(at, STREAM_SEEK_SET, nullptr))) return false;
        auto* p = static_cast<uint8_t*>(dst);
        while (n > 0) {
            const ULONG want = static_cast<ULONG>(n > (1u << 30) ? (1u << 30) : n);
            ULONG got = 0;
            if (FAILED(s_->Read(p, want, &got)) || got == 0) return false;
            p += got;
            n -= got;
        }
        return true;
    }

private:
    IStream* s_;
    uint64_t size_ = 0;
};

template <class T>
void release(T*& p) {
    if (p != nullptr) p->Release();
    p = nullptr;
}

// An encoded picture scaled to w x h, as premultiplied BGRA rows (w * 4 bytes each).
bool decode(IWICImagingFactory* wic, const std::vector<uint8_t>& bytes, UINT* src_w, UINT* src_h, UINT w, UINT h,
            std::vector<uint8_t>* out) {
    if (bytes.empty() || bytes.size() > 0xffffffffu) return false;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = false;
    do {
        if (FAILED(wic->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))))
            break;
        // A WebP takes Windows' own WebP codec (the Webp Image Extensions, in Windows 10 1809 and later).
        if (FAILED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;
        UINT fw = 0, fh = 0;
        if (FAILED(frame->GetSize(&fw, &fh)) || fw == 0 || fh == 0 || fw > 8192 || fh > 8192) break;
        if (src_w != nullptr) *src_w = fw;
        if (src_h != nullptr) *src_h = fh;
        if (w == 0 || h == 0) {
            ok = true;  // the size was all that was asked
            break;
        }
        if (FAILED(wic->CreateBitmapScaler(&scaler))) break;
        if (FAILED(scaler->Initialize(frame, w, h, WICBitmapInterpolationModeFant))) break;
        if (FAILED(wic->CreateFormatConverter(&converter))) break;
        if (FAILED(converter->Initialize(scaler, kPixelPbgra, WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)))
            break;
        out->assign(static_cast<size_t>(w) * h * 4, 0);
        ok = SUCCEEDED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(out->size()), out->data()));
    } while (false);
    release(converter);
    release(scaler);
    release(frame);
    release(decoder);
    release(stream);
    return ok;
}

// src over dst, both premultiplied BGRA of the same size.
void over(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) {
    for (size_t i = 0; i + 3 < dst.size(); i += 4) {
        const unsigned a = src[i + 3];
        for (int k = 0; k < 4; ++k) dst[i + k] = static_cast<uint8_t>(src[i + k] + dst[i + k] * (255 - a) / 255);
    }
}

// An adaptive icon's layer at s x s (108 dp), premultiplied.
bool layer_pixels(IWICImagingFactory* wic, const Layer& l, UINT s, std::vector<uint8_t>* out) {
    if (l.color) {
        const unsigned a = l.argb >> 24, r = (l.argb >> 16) & 0xff, g = (l.argb >> 8) & 0xff, b = l.argb & 0xff;
        out->resize(static_cast<size_t>(s) * s * 4);
        for (size_t i = 0; i < out->size(); i += 4) {
            (*out)[i] = static_cast<uint8_t>(b * a / 255);
            (*out)[i + 1] = static_cast<uint8_t>(g * a / 255);
            (*out)[i + 2] = static_cast<uint8_t>(r * a / 255);
            (*out)[i + 3] = static_cast<uint8_t>(a);
        }
        return true;
    }
    return decode(wic, l.image, nullptr, nullptr, s, s, out);
}

// The icon at cx x cx, premultiplied BGRA.
bool render(IWICImagingFactory* wic, const ApkIcon& icon, UINT cx, std::vector<uint8_t>* out) {
    out->assign(static_cast<size_t>(cx) * cx * 4, 0);
    if (!icon.adaptive) {
        // The picture as it is (a legacy icon is already shaped), fitted and centered.
        UINT w = 0, h = 0;
        if (!decode(wic, icon.icon.image, &w, &h, 0, 0, nullptr)) return false;
        const UINT tw = w >= h ? cx : std::max(1u, cx * w / h), th = h >= w ? cx : std::max(1u, cx * h / w);
        std::vector<uint8_t> pic;
        if (!decode(wic, icon.icon.image, nullptr, nullptr, tw, th, &pic)) return false;
        const UINT x0 = (cx - tw) / 2, y0 = (cx - th) / 2;
        for (UINT y = 0; y < th; ++y)
            std::memcpy(&(*out)[(static_cast<size_t>(y + y0) * cx + x0) * 4], &pic[static_cast<size_t>(y) * tw * 4],
                        static_cast<size_t>(tw) * 4);
        return true;
    }
    // AdaptiveIconDrawable: both layers 108 dp, the middle 72 dp shown, in AOSP's default mask (a circle,
    // config_icon_mask).
    const UINT s = (cx * 108 + 71) / 72;
    std::vector<uint8_t> full, fg;
    if (!layer_pixels(wic, icon.background, s, &full)) return false;
    if (!layer_pixels(wic, icon.foreground, s, &fg)) return false;
    over(full, fg);
    const UINT off = (s - cx) / 2;
    const double r = cx / 2.0;
    for (UINT y = 0; y < cx; ++y) {
        for (UINT x = 0; x < cx; ++x) {
            const double dx = x + 0.5 - r, dy = y + 0.5 - r;
            const double cover = std::min(1.0, std::max(0.0, r - std::sqrt(dx * dx + dy * dy) + 0.5));
            const uint8_t* p = &full[(static_cast<size_t>(y + off) * s + x + off) * 4];
            uint8_t* q = &(*out)[(static_cast<size_t>(y) * cx + x) * 4];
            for (int k = 0; k < 4; ++k) q[k] = static_cast<uint8_t>(std::lround(p[k] * cover));
        }
    }
    return true;
}

// The icon Win.apk's install rendered for this package (apps\<name>\icon.png), when installed.
bool installed_icon(const std::string& package, std::vector<uint8_t>* png) {
    if (package.empty() || package.size() > 255) return false;
    wchar_t base[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 64) return false;
    const std::wstring data = std::wstring(base, n) + L"\\Win.apk\\";
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((data + L"library\\*.package").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return false;
    std::wstring name;
    do {
        HANDLE f = CreateFileW((data + L"library\\" + fd.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        char line[300] = {};
        DWORD got = 0;
        ReadFile(f, line, sizeof(line) - 1, &got, nullptr);
        CloseHandle(f);
        size_t len = 0;
        while (len < got && line[len] != '\r' && line[len] != '\n') ++len;
        if (std::string(line, len) == package) {
            name.assign(fd.cFileName, wcslen(fd.cFileName) - 8);  // less ".package"
            break;
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    if (name.empty()) return false;
    HANDLE f = CreateFileW((data + L"apps\\" + name + L"\\icon.png").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (16 << 20);
    if (ok) {
        png->resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(f, png->data(), static_cast<DWORD>(png->size()), &got, nullptr) && got == png->size();
    }
    CloseHandle(f);
    return ok;
}

class Thumbnail final : public IInitializeWithStream, public IThumbnailProvider {
public:
    Thumbnail() { InterlockedIncrement(&g_objects); }
    ~Thumbnail() {
        release(stream_);
        InterlockedDecrement(&g_objects);
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (IsEqualIID(riid, kIidUnknown) || IsEqualIID(riid, kIidInitializeWithStream)) {
            *ppv = static_cast<IInitializeWithStream*>(this);
        } else if (IsEqualIID(riid, kIidThumbnailProvider)) {
            *ppv = static_cast<IThumbnailProvider*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return r;
    }

    // IInitializeWithStream
    STDMETHODIMP Initialize(IStream* stream, DWORD) override {
        if (stream == nullptr) return E_INVALIDARG;
        if (stream_ != nullptr) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        stream_ = stream;
        stream_->AddRef();
        return S_OK;
    }

    // IThumbnailProvider
    STDMETHODIMP GetThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) override {
        if (bitmap == nullptr || alpha == nullptr) return E_POINTER;
        *bitmap = nullptr;
        if (stream_ == nullptr) return E_UNEXPECTED;
        if (cx == 0 || cx > 2560) return E_INVALIDARG;
        ApkIcon icon;
        StreamSource source(stream_);
        bool found = winapk::shell::find_icon(source, &icon);
        IWICImagingFactory* wic = nullptr;
        if (FAILED(CoCreateInstance(kClsidWicFactory, nullptr, CLSCTX_INPROC_SERVER, kIidWicFactory,
                                    reinterpret_cast<void**>(&wic))))
            return E_FAIL;
        std::vector<uint8_t> pixels;
        bool ok = found && render(wic, icon, cx, &pixels);
        if (!ok) {
            ApkIcon installed;
            installed.package = icon.package;
            ok = installed_icon(icon.package, &installed.icon.image) && render(wic, installed, cx, &pixels);
        }
        release(wic);
        if (!ok) return E_FAIL;  // Explorer shows the file type's icon

        // A top-down 32-bit DIB, straight (not premultiplied) alpha: WTSAT_ARGB.
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = static_cast<LONG>(cx);
        bi.bmiHeader.biHeight = -static_cast<LONG>(cx);
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP h = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (h == nullptr || bits == nullptr) return E_OUTOFMEMORY;
        auto* q = static_cast<uint8_t*>(bits);
        for (size_t i = 0; i < pixels.size(); i += 4) {
            const unsigned a = pixels[i + 3];
            for (int k = 0; k < 3; ++k) q[i + k] = a == 0 ? 0 : static_cast<uint8_t>(std::min(255u, pixels[i + k] * 255u / a));
            q[i + 3] = static_cast<uint8_t>(a);
        }
        *bitmap = h;
        *alpha = WTSAT_ARGB;
        return S_OK;
    }

private:
    LONG refs_ = 1;
    IStream* stream_ = nullptr;
};

class Factory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (IsEqualIID(riid, kIidUnknown) || IsEqualIID(riid, kIidClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        *ppv = nullptr;
        if (outer != nullptr) return CLASS_E_NOAGGREGATION;
        Thumbnail* t = new (std::nothrow) Thumbnail();
        if (t == nullptr) return E_OUTOFMEMORY;
        const HRESULT hr = t->QueryInterface(riid, ppv);
        t->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL) override { return S_OK; }
};

Factory g_factory;

}  // namespace

extern "C" {

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // Never unloaded from its process (the other runtime's finding): an object's destructor lowers the count before
        // its Release has returned, and a DllCanUnloadNow answering yes in between lets COM unmap the DLL under
        // a thread still in it. The surrogate process it lives in ends on its own when idle.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (!IsEqualCLSID(clsid, kClsidThumbnail)) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

// Never: see DllMain.
STDAPI DllCanUnloadNow() { return S_FALSE; }

}  // extern "C"
