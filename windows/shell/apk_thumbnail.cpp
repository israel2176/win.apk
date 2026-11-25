// apk_thumbnail.cpp -- winapk-shell.dll: Explorer's thumbnail of an .apk, .apkm, .xapk or .apks, the app's own
// icon.
//
// A thumbnail handler as Microsoft documents it ("Building Thumbnail Handlers", IThumbnailProvider,
// IInitializeWithStream): an in-process COM server, Apartment-threaded, that the shell creates in its isolated
// surrogate process (dllhost) for a file whose type names our CLSID under ShellEx\{IThumbnailProvider's IID}. The
// shell hands over the file as a read-only IStream (Initialize, once) and asks for a bitmap that fits a cx-by-cx
// square (GetThumbnail); a failure there shows the type's icon instead.
//
// The file is parsed here (apk_icon.cpp: nothing of the APK runs) and the picture decoded with WIC. An adaptive icon
// is composed as AOSP's AdaptiveIconDrawable draws it: two 108 dp layers of which the middle 72 dp show, under
// the platform's default mask (frameworks/base core/res config_icon_mask, a circle). A vector icon, or a picture no
// installed WIC codec reads (WebP on an edition without the codec), falls back to the icon Win.apk rendered when it
// installed the same package (library\<name>.package's first line, apps\<name>\icon.png).
//
// The pixels are kept premultiplied while scaled and composed (no dark fringes where a transparent pixel's color
// would bleed in) and handed back unpremultiplied as a 32-bit top-down DIB with WTSAT_ARGB, the form the SDK's
// thumbnail sample returns. Registration (per user, under HKCU\Software\Classes): the launcher's
// register_file_types and the installer (windows/package/winapk.iss) write the same keys.
#include <windows.h>
#include <objbase.h>
#include <propsys.h>
#include <thumbcache.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "apk_icon.h"

namespace {

using winapk::shell::ApkIcon;
using winapk::shell::Layer;
using winapk::shell::Source;

// {B5A3C1E2-7D4F-4C8B-9E61-2F0A8D3C5B17}: Win.apk's own; registered by the launcher and winapk.iss.
const CLSID kClsidThumbnail = {0xb5a3c1e2, 0x7d4f, 0x4c8b, {0x9e, 0x61, 0x2f, 0x0a, 0x8d, 0x3c, 0x5b, 0x17}};

// Live objects (handlers and class factories) and IClassFactory::LockServer locks: DllCanUnloadNow's answer.
std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};

// The largest thumbnail made, in pixels. Explorer asks up to 256 by default and 1024 at most for its largest view;
// a larger cx is answered with a smaller bitmap, which the documentation allows ("this size or smaller").
constexpr UINT kMaxSide = 1024;
// A picture's largest dimension WIC is asked to decode: an icon is at most 512 px (xxxhdpi 192, the Play Store's
// 512), so anything far larger is not an icon and is not decoded at all.
constexpr UINT kMaxSourceSide = 4096;

template <class T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T** put() {
        reset();
        return &p;
    }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// The shell's IStream as apk_icon's Source. Reads go through one aligned 64 KB block kept in memory: the parser
// reads the zip's records in many small pieces, each of which would otherwise be a Seek and a Read across the
// surrogate's stream (which may be a network file).
class StreamSource final : public Source {
public:
    StreamSource(IStream* stream, uint64_t size) : stream_(stream), size_(size) {}
    uint64_t size() const override { return size_; }
    bool read(uint64_t off, void* dst, size_t n) const override {
        if (off > size_ || n > size_ - off) return false;
        auto* out = static_cast<uint8_t*>(dst);
        if (n > kBlock) return raw(off, out, n);
        while (n != 0) {
            const uint64_t base = off - off % kBlock;
            if (!have_ || base != block_off_) {
                const size_t len = static_cast<size_t>(std::min<uint64_t>(kBlock, size_ - base));
                block_.resize(len);
                have_ = raw(base, block_.data(), len);
                if (!have_) return false;
                block_off_ = base;
            }
            const size_t at = static_cast<size_t>(off - base);
            const size_t take = std::min(n, block_.size() - at);
            std::memcpy(out, block_.data() + at, take);
            out += take;
            off += take;
            n -= take;
        }
        return true;
    }

private:
    static constexpr size_t kBlock = 64 * 1024;

    // Exactly n bytes at off: IStream::Read may return fewer than asked (S_FALSE at the end, or a short read).
    bool raw(uint64_t off, uint8_t* dst, size_t n) const {
        LARGE_INTEGER to;
        to.QuadPart = static_cast<LONGLONG>(off);
        if (FAILED(stream_->Seek(to, STREAM_SEEK_SET, nullptr))) return false;
        while (n != 0) {
            const ULONG ask = static_cast<ULONG>(std::min<size_t>(n, 1u << 30));
            ULONG got = 0;
            const HRESULT hr = stream_->Read(dst, ask, &got);
            if (FAILED(hr) || got == 0) return false;
            dst += got;
            n -= got;
        }
        return true;
    }

    IStream* stream_;
    uint64_t size_;
    mutable std::vector<uint8_t> block_;
    mutable uint64_t block_off_ = 0;
    mutable bool have_ = false;
};

// A square of premultiplied BGRA pixels, row after row.
struct Canvas {
    UINT side = 0;
    std::vector<uint8_t> px;
    explicit Canvas(UINT s) : side(s), px(static_cast<size_t>(s) * s * 4, 0) {}
};

// Decodes an encoded picture (PNG, JPEG, WebP, whatever codecs Windows has) and scales it to w x h, premultiplied
// BGRA. Premultiplied before scaling: a scaler averaging straight-alpha pixels pulls the color of transparent
// pixels (often black) into the edges.
bool decode(IWICImagingFactory* wic, const std::vector<uint8_t>& bytes, UINT w, UINT h, std::vector<uint8_t>* out) {
    if (bytes.empty() || bytes.size() > 0xffffffffu || w == 0 || h == 0) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))))
        return false;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())))
        return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) return false;
    UINT sw = 0, sh = 0;
    if (FAILED(frame->GetSize(&sw, &sh)) || sw == 0 || sh == 0 || sw > kMaxSourceSide || sh > kMaxSourceSide)
        return false;
    ComPtr<IWICFormatConverter> premul;
    if (FAILED(wic->CreateFormatConverter(premul.put())) ||
        FAILED(premul->Initialize(frame.p, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                  WICBitmapPaletteTypeCustom)))
        return false;
    IWICBitmapSource* source = premul.p;
    ComPtr<IWICBitmapScaler> scaler;
    if (sw != w || sh != h) {
        if (FAILED(wic->CreateBitmapScaler(scaler.put()))) return false;
        // Fant (a box filter) where the picture shrinks a lot, as icons usually do; cubic where it grows or barely
        // shrinks. HighQualityCubic is Windows 10's; a scaler without it (Wine) is asked again with Fant.
        const bool shrink = sw >= 2 * w || sh >= 2 * h;
        // (Its value is 4 in Microsoft's documentation; older MinGW headers lack the name.)
        const auto high_quality_cubic = static_cast<WICBitmapInterpolationMode>(4);
        const WICBitmapInterpolationMode mode = shrink ? WICBitmapInterpolationModeFant : high_quality_cubic;
        if (FAILED(scaler->Initialize(premul.p, w, h, mode)) &&
            FAILED(scaler->Initialize(premul.p, w, h, WICBitmapInterpolationModeFant)))
            return false;
        source = scaler.p;
    }
    out->assign(static_cast<size_t>(w) * h * 4, 0);
    return SUCCEEDED(source->CopyPixels(nullptr, w * 4, static_cast<UINT>(out->size()), out->data()));
}

// Porter-Duff "source over" of premultiplied src (sw x sw, at x, y; may stick out) onto the canvas.
void draw_over(Canvas* dst, const std::vector<uint8_t>& src, UINT sw, int x, int y) {
    for (UINT r = 0; r < sw; ++r) {
        const int dy = y + static_cast<int>(r);
        if (dy < 0 || dy >= static_cast<int>(dst->side)) continue;
        for (UINT c = 0; c < sw; ++c) {
            const int dx = x + static_cast<int>(c);
            if (dx < 0 || dx >= static_cast<int>(dst->side)) continue;
            const uint8_t* s = &src[(static_cast<size_t>(r) * sw + c) * 4];
            uint8_t* d = &dst->px[(static_cast<size_t>(dy) * dst->side + dx) * 4];
            const unsigned keep = 255 - s[3];
            for (int k = 0; k < 4; ++k) d[k] = static_cast<uint8_t>(s[k] + (d[k] * keep + 127) / 255);
        }
    }
}

void fill(Canvas* dst, uint32_t argb) {
    const unsigned a = argb >> 24;
    const uint8_t pre[4] = {static_cast<uint8_t>(((argb & 0xff) * a + 127) / 255),
                            static_cast<uint8_t>((((argb >> 8) & 0xff) * a + 127) / 255),
                            static_cast<uint8_t>((((argb >> 16) & 0xff) * a + 127) / 255), static_cast<uint8_t>(a)};
    std::vector<uint8_t> one(static_cast<size_t>(dst->side) * dst->side * 4);
    for (size_t i = 0; i < one.size(); i += 4) std::memcpy(&one[i], pre, 4);
    draw_over(dst, one, dst->side, 0, 0);
}

// An adaptive icon's layer onto the canvas: the layer is 108 dp of which the middle 72 fill the canvas, so it is
// drawn at 1.5 times the canvas, centered, its outer sixth on each side cut off.
bool draw_layer(IWICImagingFactory* wic, const Layer& layer, Canvas* dst) {
    if (layer.empty()) return true;
    if (layer.color) {
        fill(dst, layer.argb);
        return true;
    }
    const UINT big = (dst->side * 3 + 1) / 2;
    std::vector<uint8_t> px;
    if (!decode(wic, layer.image, big, big, &px)) return false;
    const int at = -static_cast<int>((big - dst->side) / 2);
    draw_over(dst, px, big, at, at);
    return true;
}

// The mask over the composed layers: AOSP's default config_icon_mask, a circle filling the icon, with its edge
// anti-aliased by the pixel's coverage (4 x 4 samples).
void mask_circle(Canvas* c) {
    const double r = c->side / 2.0;
    for (UINT y = 0; y < c->side; ++y) {
        for (UINT x = 0; x < c->side; ++x) {
            const double dx = x + 0.5 - r, dy = y + 0.5 - r;
            const double d = std::sqrt(dx * dx + dy * dy);
            unsigned cover;
            if (d <= r - 1.0) continue;
            if (d >= r + 1.0) {
                cover = 0;
            } else {
                int in = 0;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx) {
                        const double px = x + (sx + 0.5) / 4 - r, py = y + (sy + 0.5) / 4 - r;
                        if (px * px + py * py <= r * r) ++in;
                    }
                cover = static_cast<unsigned>(in * 255 / 16);
            }
            uint8_t* p = &c->px[(static_cast<size_t>(y) * c->side + x) * 4];
            for (int k = 0; k < 4; ++k) p[k] = static_cast<uint8_t>((p[k] * cover + 127) / 255);
        }
    }
}

// A plain picture, scaled to fit the canvas with its aspect kept, centered.
bool draw_picture(IWICImagingFactory* wic, const std::vector<uint8_t>& bytes, Canvas* dst) {
    // The picture's size first, for the aspect: a second decoder over the same bytes is cheap (the header only).
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT sw = 0, sh = 0;
    if (bytes.empty() || bytes.size() > 0xffffffffu || FAILED(wic->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) ||
        FAILED(wic->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())) || FAILED(frame->GetSize(&sw, &sh)) || sw == 0 || sh == 0)
        return false;
    const double scale = static_cast<double>(dst->side) / std::max(sw, sh);
    const UINT w = std::max<UINT>(1, static_cast<UINT>(std::lround(sw * scale)));
    const UINT h = std::max<UINT>(1, static_cast<UINT>(std::lround(sh * scale)));
    std::vector<uint8_t> px;
    if (!decode(wic, bytes, w, h, &px)) return false;
    // Into a square of its own, so draw_over's square walk applies.
    const UINT side = std::max(w, h);
    std::vector<uint8_t> sq(static_cast<size_t>(side) * side * 4, 0);
    const UINT ox = (side - w) / 2, oy = (side - h) / 2;
    for (UINT r = 0; r < h; ++r)
        std::memcpy(&sq[((static_cast<size_t>(r) + oy) * side + ox) * 4], &px[static_cast<size_t>(r) * w * 4], w * 4);
    draw_over(dst, sq, side, (static_cast<int>(dst->side) - static_cast<int>(side)) / 2,
              (static_cast<int>(dst->side) - static_cast<int>(side)) / 2);
    return true;
}

bool render(IWICImagingFactory* wic, const ApkIcon& icon, Canvas* c) {
    if (!icon.adaptive) return draw_picture(wic, icon.icon.image, c);
    if (!draw_layer(wic, icon.background, c) || !draw_layer(wic, icon.foreground, c)) return false;
    mask_circle(c);
    return true;
}

// The bytes of a file, when it is no larger than max.
bool read_file(const std::wstring& path, size_t max, std::vector<uint8_t>* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && static_cast<uint64_t>(size.QuadPart) <= max;
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(f, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) && got == out->size();
    }
    CloseHandle(f);
    return ok;
}

// The icon Win.apk rendered at the install of the same package: the library app whose .package file's first line
// is the package, then its apps\<name>\icon.png. %LOCALAPPDATA%\Win.apk is the launcher's data_dir().
bool installed_icon(const std::string& package, std::vector<uint8_t>* png) {
    if (package.empty() || package.size() > 255) return false;
    wchar_t base[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return false;
    const std::wstring data = std::wstring(base, n) + L"\\Win.apk\\";
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((data + L"library\\*.package").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::vector<uint8_t> text;
        if (!read_file(data + L"library\\" + fd.cFileName, 64 * 1024, &text)) continue;
        size_t end = 0;
        while (end < text.size() && text[end] != '\n' && text[end] != '\r') ++end;
        if (std::string(text.begin(), text.begin() + end) != package) continue;
        std::wstring name = fd.cFileName;
        name.resize(name.size() - std::wcslen(L".package"));
        found = read_file(data + L"apps\\" + name + L"\\icon.png", 4 * 1024 * 1024, png);
    } while (!found && FindNextFileW(find, &fd));
    FindClose(find);
    return found;
}

// A 32-bit top-down DIB of the canvas, unpremultiplied (WTSAT_ARGB).
HBITMAP to_bitmap(const Canvas& c) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = static_cast<LONG>(c.side);
    bi.bmiHeader.biHeight = -static_cast<LONG>(c.side);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) return nullptr;
    auto* out = static_cast<uint8_t*>(bits);
    for (size_t i = 0; i < c.px.size(); i += 4) {
        const unsigned a = c.px[i + 3];
        for (int k = 0; k < 3; ++k)
            out[i + k] = a == 0 ? 0 : static_cast<uint8_t>(std::min(255u, (c.px[i + k] * 255u + a / 2) / a));
        out[i + 3] = static_cast<uint8_t>(a);
    }
    return bmp;
}

class ThumbnailProvider final : public IThumbnailProvider, public IInitializeWithStream {
public:
    ThumbnailProvider() { ++g_objects; }
    ThumbnailProvider(const ThumbnailProvider&) = delete;
    ThumbnailProvider& operator=(const ThumbnailProvider&) = delete;

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IThumbnailProvider))) {
            *ppv = static_cast<IThumbnailProvider*>(this);
        } else if (IsEqualIID(riid, __uuidof(IInitializeWithStream))) {
            *ppv = static_cast<IInitializeWithStream*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const long left = --refs_;
        if (left == 0) delete this;
        return static_cast<ULONG>(left);
    }

    // IInitializeWithStream: called once, before GetThumbnail; the stream is read-only and stays the shell's.
    STDMETHODIMP Initialize(IStream* stream, DWORD) override {
        if (!stream) return E_INVALIDARG;
        if (stream_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        stream_ = stream;
        stream_->AddRef();
        return S_OK;
    }

    // IThumbnailProvider
    STDMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* alpha) override {
        if (!phbmp || !alpha) return E_POINTER;
        *phbmp = nullptr;
        *alpha = WTSAT_UNKNOWN;
        if (!stream_) return E_UNEXPECTED;
        if (cx == 0) return E_INVALIDARG;
        STATSTG st = {};
        if (FAILED(stream_->Stat(&st, STATFLAG_NONAME))) return E_FAIL;
        StreamSource file(stream_, st.cbSize.QuadPart);
        ApkIcon icon;
        const bool found = winapk::shell::find_icon(file, &icon);

        ComPtr<IWICImagingFactory> wic;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    __uuidof(IWICImagingFactory), reinterpret_cast<void**>(wic.put()))))
            return E_FAIL;
        Canvas canvas(std::min(cx, kMaxSide));
        bool drawn = found && render(wic.p, icon, &canvas);
        if (!drawn) {
            std::vector<uint8_t> png;
            if (installed_icon(icon.package, &png)) {
                canvas = Canvas(canvas.side);
                drawn = draw_picture(wic.p, png, &canvas);
            }
        }
        if (!drawn) return E_FAIL;
        HBITMAP bmp = to_bitmap(canvas);
        if (!bmp) return E_OUTOFMEMORY;
        *phbmp = bmp;
        *alpha = WTSAT_ARGB;
        return S_OK;
    }

private:
    ~ThumbnailProvider() {
        if (stream_) stream_->Release();
        --g_objects;
    }
    std::atomic<long> refs_{1};
    IStream* stream_ = nullptr;
};

class ClassFactory final : public IClassFactory {
public:
    ClassFactory() { ++g_objects; }
    ClassFactory(const ClassFactory&) = delete;
    ClassFactory& operator=(const ClassFactory&) = delete;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const long left = --refs_;
        if (left == 0) delete this;
        return static_cast<ULONG>(left);
    }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* p = new (std::nothrow) ThumbnailProvider();
        if (!p) return E_OUTOFMEMORY;
        const HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock)
            ++g_locks;
        else
            --g_locks;
        return S_OK;
    }

private:
    ~ClassFactory() { --g_objects; }
    std::atomic<long> refs_{1};
};

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!IsEqualCLSID(clsid, kClsidThumbnail)) return CLASS_E_CLASSNOTAVAILABLE;
    auto* f = new (std::nothrow) ClassFactory();
    if (!f) return E_OUTOFMEMORY;
    const HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

// COM asks this (CoFreeUnusedLibraries) on the apartment that loaded the DLL, the same thread every one of its
// Apartment-threaded objects is released on, so a zero count here cannot race a Release still returning.
STDAPI DllCanUnloadNow() { return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE; }
