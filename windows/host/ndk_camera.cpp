// ndk_camera.cpp -- libcamera2ndk (ACameraManager, ACameraDevice, ACameraCaptureSession,
// ACaptureRequest, ACameraMetadata) and libmediandk's AImageReader and AImage for an app's
// native code, on the PC's webcams (camera_host.h: native_camera.cpp's Media Foundation
// sessions, the cameras Java's camera2 sees, numbered alike: "0" is the first webcam's back
// role, "1" its front one).
//
// What a device does, kept:
//  - the characteristics are those CameraBackend.java gives camera2 (LEGACY hardware level,
//    BACKWARD_COMPATIBLE, the webcam's sizes for YUV_420_888, JPEG and PRIVATE outputs, its
//    frame-rate ranges, autofocus and torch where the webcam has them), in NdkCameraMetadataTags'
//    tags and types; JPEG is AIMAGE_FORMAT_JPEG in the stream configurations, as ACameraMetadata
//    rewrites the HAL's BLOB;
//  - openCamera wants the CAMERA permission (ACAMERA_ERROR_PERMISSION_DENIED) and Windows' camera
//    privacy switch (ACAMERA_ERROR_CAMERA_DISABLED); one session per device, a new one closes the
//    old (onClosed); onReady once configured and whenever no request is left, onActive when one
//    starts;
//  - setRepeatingRequest and capture: every frame of the webcam serves the next request, its
//    targets get the picture (an AImageReader's window: YUV_420_888 as the camera gives it,
//    RGBA_8888/RGBX_8888 or a JPEG at the request's JPEG_QUALITY, scaled to the reader's size;
//    the runtime's native window: shown), then onCaptureStarted with the frame's timestamp and
//    onCaptureCompleted with a result holding the request's settings and SENSOR_TIMESTAMP, the
//    3A states and the lens state; onCaptureSequenceCompleted when a sequence ends; the
//    callbacks run in order on a thread of the device's own, with no lock held;
//  - CONTROL_AF_MODE continuous or off steers the webcam's autofocus, FLASH_MODE_TORCH its torch;
//  - AImageReader: maxImages buffers, onImageAvailable on that thread, acquireNextImage /
//    acquireLatestImage with NO_BUFFER_AVAILABLE and MAX_IMAGES_ACQUIRED as Android answers;
//    YUV_420_888 is semiplanar (the U and V planes one byte apart, pixel stride 2), as camera2's
//    Image from CameraBridge is.
// What differs: the camera takes one size, the largest the session's readers ask for (a
// smaller reader gets that picture scaled), and settings it has no control for (exposure,
// white balance, zoom, regions) are kept in the result but change nothing, as the LEGACY level
// says; no reprocessing, no physical cameras, no shared outputs, no session parameters.
// An ANativeWindow that is neither a reader's nor the runtime's window (an ASurfaceTexture's)
// gets nothing.

#include "ndk_camera.h"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "camera_host.h"
#include "native_window.h"
#include "winapk/arm64_guest.h"
#include "winapk/guest_abi.h"

namespace winapk::host {

namespace {

#define GUEST WINAPK_GUEST_ABI

// camera_status_t (NdkCameraError.h) and media_status_t (NdkMediaError.h).
constexpr int kOk = 0, kErrInvalidParameter = -10001, kErrMetadataNotFound = -10004,
              kErrSessionClosed = -10007, kErrStreamConfigureFail = -10009,
              kErrCameraDisabled = -10012, kErrPermissionDenied = -10013, kErrUnsupportedOperation = -10014;
constexpr int kMediaOk = 0, kMediaErrInvalidParameter = -10004, kMediaErrInvalidObject = -10003,
              kMediaErrUnsupported = -10002, kNoBufferAvailable = -30001, kMaxImagesAcquired = -30002;

// NdkImage.h.
constexpr int32_t kRgba8888 = 0x1, kRgbx8888 = 0x2, kYuv420888 = 0x23, kJpeg = 0x100, kPrivate = 0x22;

// NdkCameraMetadataTags.h (AOSP 14).
enum Tag : uint32_t {
    kAeAntibandingMode = 0x10000, kAeMode = 0x10003, kAeTargetFpsRange = 0x10005, kAePrecaptureTrigger = 0x10006,
    kAfMode = 0x10007, kAfTrigger = 0x10009, kAwbMode = 0x1000b, kCaptureIntent = 0x1000d, kControlMode = 0x1000f,
    kAeAvailableAntibanding = 0x10012, kAeAvailableModes = 0x10013, kAeAvailableFpsRanges = 0x10014,
    kAeCompensationRange = 0x10015, kAeCompensationStep = 0x10016, kAfAvailableModes = 0x10017,
    kAwbAvailableModes = 0x1001b, kMaxRegions = 0x1001c, kAeState = 0x1001f, kAfState = 0x10020,
    kAwbState = 0x10022, kAeLockAvailable = 0x10024, kAwbLockAvailable = 0x10025, kAvailableModes = 0x10026,
    kFlashMode = 0x40002, kFlashState = 0x40005, kFlashInfoAvailable = 0x50000,
    kJpegOrientation = 0x70003, kJpegQuality = 0x70004, kJpegThumbnailSize = 0x70006,
    kJpegAvailableThumbnailSizes = 0x70007, kLensFacing = 0x80005, kLensState = 0x80009,
    kLensMinimumFocusDistance = 0x90005, kMaxNumOutputStreams = 0xc0006, kPipelineMaxDepth = 0xc000a,
    kPartialResultCount = 0xc000b, kAvailableCapabilities = 0xc000c, kAvailableRequestKeys = 0xc000d,
    kAvailableResultKeys = 0xc000e, kAvailableCharacteristicsKeys = 0xc000f, kCropRegion = 0xd0000,
    kMaxDigitalZoom = 0xd0004, kStreamConfigurations = 0xd000a, kMinFrameDurations = 0xd000b,
    kStallDurations = 0xd000c, kCroppingType = 0xd000d, kSensorFrameDuration = 0xe0001,
    kSensorOrientation = 0xe000e, kSensorTimestamp = 0xe0010, kActiveArraySize = 0xf0000,
    kPixelArraySize = 0xf0006, kTimestampSource = 0xf0008, kPreCorrectionActiveArraySize = 0xf000a,
    kFaceDetectMode = 0x110000, kAvailableFaceDetectModes = 0x120000, kHardwareLevel = 0x150000,
    kSyncMaxLatency = 0x170001,
};
enum Type : uint8_t { kByte = 0, kInt32 = 1, kFloat = 2, kInt64 = 3, kDouble = 4, kRational = 5 };

template <typename F>
F guest_fn(F fn) {
    return reinterpret_cast<F>(fn ? arm64::host_callable(reinterpret_cast<void*>(fn)) : nullptr);
}

void log_once(const char* what, const std::string& detail) {
    static std::mutex lock;
    static std::set<std::string> said;
    std::lock_guard<std::mutex> hold(lock);
    if (!said.insert(std::string(what) + detail).second) return;
    std::fprintf(stderr, "[camera] NDK: %s%s%s\n", what, detail.empty() ? "" : " ", detail.c_str());
    std::fflush(stderr);
}

// ---- ACameraMetadata ---------------------------------------------------------------------------

struct ConstEntry {  // ACameraMetadata_const_entry
    uint32_t tag;
    uint8_t type;
    uint32_t count;
    const void* data;
};

struct Metadata {
    struct Entry {
        uint8_t type = kByte;
        uint32_t count = 0;
        std::vector<uint8_t> bytes;
    };
    std::map<uint32_t, Entry> entries;
    std::vector<uint32_t> tags;  // getAllTags' answer, valid until the next change

    static size_t size_of(uint8_t type) {
        switch (type) {
            case kByte: return 1;
            case kInt32: case kFloat: return 4;
            default: return 8;  // int64, double, rational
        }
    }
    void set(uint32_t tag, uint8_t type, uint32_t count, const void* data) {
        if (count == 0 || !data) {
            entries.erase(tag);
            return;
        }
        Entry& e = entries[tag];
        e.type = type;
        e.count = count;
        const auto* p = static_cast<const uint8_t*>(data);
        e.bytes.assign(p, p + size_of(type) * count);
    }
    void u8(uint32_t tag, std::initializer_list<uint8_t> v) { set(tag, kByte, uint32_t(v.size()), v.begin()); }
    void i32(uint32_t tag, const std::vector<int32_t>& v) { set(tag, kInt32, uint32_t(v.size()), v.data()); }
    void i64(uint32_t tag, const std::vector<int64_t>& v) { set(tag, kInt64, uint32_t(v.size()), v.data()); }
    void f32(uint32_t tag, std::initializer_list<float> v) { set(tag, kFloat, uint32_t(v.size()), v.begin()); }
    void rational(uint32_t tag, int32_t num, int32_t den) {
        const int32_t r[2] = {num, den};
        set(tag, kRational, 1, r);
    }
    int get(uint32_t tag, ConstEntry* out) const {
        auto it = entries.find(tag);
        if (it == entries.end()) return kErrMetadataNotFound;
        out->tag = tag;
        out->type = it->second.type;
        out->count = it->second.count;
        out->data = it->second.bytes.data();
        return kOk;
    }
    const Entry* find(uint32_t tag, uint8_t type) const {
        auto it = entries.find(tag);
        return it != entries.end() && it->second.type == type && it->second.count > 0 ? &it->second : nullptr;
    }
    int32_t int32_or(uint32_t tag, int32_t fallback, uint32_t index = 0) const {
        const Entry* e = find(tag, kInt32);
        if (!e || index >= e->count) return fallback;
        int32_t v;
        std::memcpy(&v, e->bytes.data() + 4 * index, 4);
        return v;
    }
    int byte_or(uint32_t tag, int fallback) const {
        const Entry* e = find(tag, kByte);
        return e ? e->bytes[0] : fallback;
    }
    const std::vector<uint32_t>& all_tags() {
        tags.clear();
        for (const auto& [tag, e] : entries) tags.push_back(tag);
        return tags;
    }
};

// ---- dispatch threads -----------------------------------------------------------------------------

// One thread running posted work in order: where a device's callbacks, its readers' listeners and
// the manager's availability callbacks run, with no lock of ours held.
class Dispatcher {
public:
    Dispatcher() : thread_([this] { run(); }) {}
    // Runs what was posted, then ends. Never from its own thread: see finish().
    ~Dispatcher() {
        finish();
        thread_.join();
    }
    // Ends the thread once what was posted has run, without waiting: for a device closed from one of
    // its own callbacks, whose dispatcher is then let go of (it ends by itself and is not freed).
    void finish() {
        {
            std::lock_guard<std::mutex> hold(lock_);
            stop_ = true;
        }
        cv_.notify_all();
    }
    void post(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> hold(lock_);
            work_.push_back(std::move(fn));
        }
        cv_.notify_all();
    }
    bool on_thread() const { return thread_.get_id() == std::this_thread::get_id(); }

private:
    void run() {
        HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // WIC's JPEG encoder
        std::unique_lock<std::mutex> hold(lock_);
        for (;;) {
            cv_.wait(hold, [this] { return stop_ || !work_.empty(); });
            if (work_.empty()) break;
            std::function<void()> fn = std::move(work_.front());
            work_.pop_front();
            hold.unlock();
            fn();
            hold.lock();
        }
        hold.unlock();
        if (SUCCEEDED(com)) CoUninitialize();
    }
    std::mutex lock_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> work_;
    bool stop_ = false;
    std::thread thread_;
};

// ---- pictures ---------------------------------------------------------------------------------------

// One image's memory and its planes' layout (AImage's).
struct ImageBuffer {
    int32_t format = 0, width = 0, height = 0;
    int64_t timestamp = 0;
    std::vector<uint8_t> bytes;
    struct Plane {
        size_t offset = 0, length = 0;
        int32_t pixel_stride = 0, row_stride = 0;
    };
    std::vector<Plane> planes;
};

uint8_t clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

// The NV12 picture scaled (nearest) to w x h, NV12 with stride w.
std::vector<uint8_t> scale_nv12(const camera::Picture& p, int w, int h) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h + static_cast<size_t>(w) * (h / 2));
    const uint8_t* uv = p.nv12 + static_cast<size_t>(p.stride) * p.height;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = p.nv12 + static_cast<size_t>(p.stride) * (static_cast<int64_t>(y) * p.height / h);
        uint8_t* d = out.data() + static_cast<size_t>(w) * y;
        for (int x = 0; x < w; ++x) d[x] = row[static_cast<int64_t>(x) * p.width / w];
    }
    uint8_t* duv = out.data() + static_cast<size_t>(w) * h;
    const int cw = w / 2, ch = h / 2, sw = std::max(1, p.width / 2), sh = std::max(1, p.height / 2);
    for (int y = 0; y < ch; ++y) {
        const uint8_t* row = uv + static_cast<size_t>(p.stride) * (static_cast<int64_t>(y) * sh / ch);
        uint8_t* d = duv + static_cast<size_t>(w) * y;
        for (int x = 0; x < cw; ++x) {
            const int sx = static_cast<int>(static_cast<int64_t>(x) * sw / cw);
            d[2 * x] = row[2 * sx];
            d[2 * x + 1] = row[2 * sx + 1];
        }
    }
    return out;
}

// Full-range BT.601 (JFIF) Y'CbCr of an NV12 picture with stride w to R, G, B.
template <typename Put>
void nv12_to_rgb(const uint8_t* nv12, int w, int h, Put put) {
    const uint8_t* uv = nv12 + static_cast<size_t>(w) * h;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int Y = nv12[static_cast<size_t>(w) * y + x];
            const uint8_t* c = uv + static_cast<size_t>(w) * (y / 2) + 2 * (x / 2);
            const int cb = c[0] - 128, cr = c[1] - 128;
            put(x, y, clamp8(Y + ((91881 * cr) >> 16)), clamp8(Y - ((22554 * cb + 46802 * cr) >> 16)),
                clamp8(Y + ((116130 * cb) >> 16)));
        }
    }
}

// A JPEG of the NV12 picture (stride w), at `quality` (1-100), by WIC; empty on failure.
std::vector<uint8_t> encode_jpeg(const std::vector<uint8_t>& nv12, int w, int h, int quality) {
    std::vector<uint8_t> bgr(static_cast<size_t>(w) * h * 3);
    nv12_to_rgb(nv12.data(), w, h, [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* d = bgr.data() + (static_cast<size_t>(w) * y + x) * 3;
        d[0] = b, d[1] = g, d[2] = r;
    });
    std::vector<uint8_t> out;
    IWICImagingFactory* factory = nullptr;
    IStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) &&
              SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) &&
              SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(encoder->CreateNewFrame(&frame, &props));
    if (ok) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = std::clamp(quality, 1, 100) / 100.0f;
        props->Write(1, &option, &value);
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        ok = SUCCEEDED(frame->Initialize(props)) && SUCCEEDED(frame->SetSize(w, h)) &&
             SUCCEEDED(frame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat24bppBGR &&
             SUCCEEDED(frame->WritePixels(h, w * 3, static_cast<UINT>(bgr.size()), bgr.data())) &&
             SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
    if (ok) {
        STATSTG stat{};
        LARGE_INTEGER zero{};
        if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) {
            out.resize(static_cast<size_t>(stat.cbSize.QuadPart));
            ULONG got = 0;
            if (FAILED(stream->Read(out.data(), static_cast<ULONG>(out.size()), &got)) || got != out.size()) out.clear();
        }
    }
    if (props) props->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    return out;
}

// The picture as a reader of this format and size takes it; null for a format there is none of.
std::shared_ptr<ImageBuffer> make_image(const camera::Picture& p, int32_t format, int w, int h, int quality) {
    auto img = std::make_shared<ImageBuffer>();
    img->format = format;
    img->width = w;
    img->height = h;
    img->timestamp = p.timestamp_ns;
    std::vector<uint8_t> nv12 = scale_nv12(p, w, h);
    const size_t luma = static_cast<size_t>(w) * h, chroma = static_cast<size_t>(w) * (h / 2);
    switch (format) {
        case kYuv420888:
            img->bytes = std::move(nv12);
            img->planes = {{0, luma, 1, w}, {luma, chroma - 1, 2, w}, {luma + 1, chroma - 1, 2, w}};
            return img;
        case kRgba8888:
        case kRgbx8888:
            img->bytes.resize(luma * 4);
            nv12_to_rgb(nv12.data(), w, h, [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
                uint8_t* d = img->bytes.data() + (static_cast<size_t>(w) * y + x) * 4;
                d[0] = r, d[1] = g, d[2] = b, d[3] = 0xff;
            });
            img->planes = {{0, img->bytes.size(), 4, w * 4}};
            return img;
        case kPrivate:  // the GPU's buffers on a device: no plane the CPU may read
            return img;
        case kJpeg:
            img->bytes = encode_jpeg(nv12, w, h, quality);
            if (img->bytes.empty()) return nullptr;
            img->planes = {{0, img->bytes.size(), 0, 0}};
            return img;
        default:
            return nullptr;
    }
}

// ---- AImageReader, AImage ----------------------------------------------------------------------

struct Reader;
struct ReaderWindow {
    uint32_t magic = 0x52574e41;  // "ANWR": a reader's window, not the runtime's
    Reader* reader = nullptr;
};

struct ImageListener {  // AImageReader_ImageListener
    void* context;
    void(GUEST* onImageAvailable)(void* context, Reader* reader);
};

struct Reader : std::enable_shared_from_this<Reader> {
    int32_t width = 0, height = 0, format = 0, max_images = 1;
    ReaderWindow window;
    std::mutex lock;
    std::deque<std::shared_ptr<ImageBuffer>> queue;
    int acquired = 0;
    ImageListener listener{};
};

struct Image {
    std::shared_ptr<ImageBuffer> buffer;
    std::weak_ptr<Reader> reader;
};

std::mutex g_readers_lock;
std::map<const void*, std::shared_ptr<Reader>> g_readers;  // by window, and by reader pointer

std::shared_ptr<Reader> reader_of(const void* p) {
    std::lock_guard<std::mutex> hold(g_readers_lock);
    auto it = g_readers.find(p);
    return it == g_readers.end() ? nullptr : it->second;
}

GUEST int AImageReader_newWithUsage(int32_t w, int32_t h, int32_t format, uint64_t, int32_t max_images,
                                    Reader** out) {
    if (!out || w < 1 || h < 1 || max_images < 1) return kMediaErrInvalidParameter;
    if (format != kYuv420888 && format != kRgba8888 && format != kRgbx8888 && format != kJpeg && format != kPrivate) {
        log_once("AImageReader_new: a format the camera does not give:", std::to_string(format));
        return kMediaErrUnsupported;
    }
    auto r = std::make_shared<Reader>();
    r->width = w, r->height = h, r->format = format, r->max_images = max_images;
    r->window.reader = r.get();
    {
        std::lock_guard<std::mutex> hold(g_readers_lock);
        g_readers[r.get()] = r;
        g_readers[&r->window] = r;
    }
    *out = r.get();
    return kMediaOk;
}
GUEST int AImageReader_new(int32_t w, int32_t h, int32_t format, int32_t max_images, Reader** out) {
    return AImageReader_newWithUsage(w, h, format, 0, max_images, out);
}
GUEST void AImageReader_delete(Reader* r) {
    std::lock_guard<std::mutex> hold(g_readers_lock);
    auto it = g_readers.find(r);
    if (it == g_readers.end()) return;
    g_readers.erase(&it->second->window);
    g_readers.erase(it);
}
GUEST int AImageReader_getWindow(Reader* r, void** window) {
    if (!r || !window) return kMediaErrInvalidParameter;
    *window = &r->window;
    return kMediaOk;
}
GUEST int AImageReader_getWidth(const Reader* r, int32_t* v) {
    if (!r || !v) return kMediaErrInvalidParameter;
    *v = r->width;
    return kMediaOk;
}
GUEST int AImageReader_getHeight(const Reader* r, int32_t* v) {
    if (!r || !v) return kMediaErrInvalidParameter;
    *v = r->height;
    return kMediaOk;
}
GUEST int AImageReader_getFormat(const Reader* r, int32_t* v) {
    if (!r || !v) return kMediaErrInvalidParameter;
    *v = r->format;
    return kMediaOk;
}
GUEST int AImageReader_getMaxImages(const Reader* r, int32_t* v) {
    if (!r || !v) return kMediaErrInvalidParameter;
    *v = r->max_images;
    return kMediaOk;
}
GUEST int AImageReader_setImageListener(Reader* r, ImageListener* listener) {
    if (!r) return kMediaErrInvalidParameter;
    std::lock_guard<std::mutex> hold(r->lock);
    r->listener = listener ? ImageListener{listener->context, guest_fn(listener->onImageAvailable)} : ImageListener{};
    return kMediaOk;
}
GUEST int AImageReader_setBufferRemovedListener(Reader* r, void*) { return r ? kMediaOk : kMediaErrInvalidParameter; }

int acquire(Reader* r, Image** out, bool latest) {
    if (!r || !out) return kMediaErrInvalidParameter;
    auto keep = reader_of(r);
    if (!keep) return kMediaErrInvalidObject;
    std::lock_guard<std::mutex> hold(r->lock);
    if (r->acquired >= r->max_images) return kMaxImagesAcquired;
    if (r->queue.empty()) return kNoBufferAvailable;
    if (latest) {
        while (r->queue.size() > 1) r->queue.pop_front();
    }
    auto* img = new Image{std::move(r->queue.front()), keep};
    r->queue.pop_front();
    ++r->acquired;
    *out = img;
    return kMediaOk;
}
GUEST int AImageReader_acquireNextImage(Reader* r, Image** out) { return acquire(r, out, false); }
GUEST int AImageReader_acquireLatestImage(Reader* r, Image** out) { return acquire(r, out, true); }
GUEST int AImageReader_acquireNextImageAsync(Reader* r, Image** out, int* fence) {
    if (fence) *fence = -1;
    return acquire(r, out, false);
}
GUEST int AImageReader_acquireLatestImageAsync(Reader* r, Image** out, int* fence) {
    if (fence) *fence = -1;
    return acquire(r, out, true);
}

GUEST void AImage_delete(Image* img) {
    if (!img) return;
    if (auto r = img->reader.lock()) {
        std::lock_guard<std::mutex> hold(r->lock);
        if (r->acquired > 0) --r->acquired;
    }
    delete img;
}
GUEST void AImage_deleteAsync(Image* img, int) { AImage_delete(img); }
GUEST int AImage_getWidth(const Image* img, int32_t* v) {
    if (!img || !v) return kMediaErrInvalidParameter;
    *v = img->buffer->width;
    return kMediaOk;
}
GUEST int AImage_getHeight(const Image* img, int32_t* v) {
    if (!img || !v) return kMediaErrInvalidParameter;
    *v = img->buffer->height;
    return kMediaOk;
}
GUEST int AImage_getFormat(const Image* img, int32_t* v) {
    if (!img || !v) return kMediaErrInvalidParameter;
    *v = img->buffer->format;
    return kMediaOk;
}
GUEST int AImage_getCropRect(const Image* img, int32_t* rect) {
    if (!img || !rect) return kMediaErrInvalidParameter;
    rect[0] = 0, rect[1] = 0, rect[2] = img->buffer->width, rect[3] = img->buffer->height;
    return kMediaOk;
}
GUEST int AImage_getTimestamp(const Image* img, int64_t* v) {
    if (!img || !v) return kMediaErrInvalidParameter;
    *v = img->buffer->timestamp;
    return kMediaOk;
}
GUEST int AImage_getNumberOfPlanes(const Image* img, int32_t* v) {
    if (!img || !v) return kMediaErrInvalidParameter;
    *v = static_cast<int32_t>(img->buffer->planes.size());
    return kMediaOk;
}
const ImageBuffer::Plane* plane_of(const Image* img, int index) {
    return img && index >= 0 && static_cast<size_t>(index) < img->buffer->planes.size()
               ? &img->buffer->planes[static_cast<size_t>(index)]
               : nullptr;
}
GUEST int AImage_getPlanePixelStride(const Image* img, int index, int32_t* v) {
    const ImageBuffer::Plane* p = plane_of(img, index);
    if (!p || !v) return kMediaErrInvalidParameter;
    if (img->buffer->format == kJpeg) return kMediaErrUnsupported;
    *v = p->pixel_stride;
    return kMediaOk;
}
GUEST int AImage_getPlaneRowStride(const Image* img, int index, int32_t* v) {
    const ImageBuffer::Plane* p = plane_of(img, index);
    if (!p || !v) return kMediaErrInvalidParameter;
    if (img->buffer->format == kJpeg) return kMediaErrUnsupported;
    *v = p->row_stride;
    return kMediaOk;
}
GUEST int AImage_getPlaneData(const Image* img, int index, uint8_t** data, int* length) {
    const ImageBuffer::Plane* p = plane_of(img, index);
    if (!p || !data || !length) return kMediaErrInvalidParameter;
    *data = img->buffer->bytes.data() + p->offset;
    *length = static_cast<int>(p->length);
    return kMediaOk;
}
GUEST int AImage_getHardwareBuffer(const Image* img, void** buffer) {
    if (buffer) *buffer = nullptr;
    return img ? kMediaErrUnsupported : kMediaErrInvalidParameter;
}
GUEST int AImage_getDataSpace(const Image* img, int32_t* v) {
    if (!img || !v) return kMediaErrInvalidParameter;
    *v = 146931712;  // ADATASPACE_JFIF: full-range BT.601, as the webcam's pictures are

    return kMediaOk;
}

// ---- requests, outputs ----------------------------------------------------------------------------

struct Request {  // ACaptureRequest
    Metadata settings;
    std::vector<const void*> targets;  // windows
    void* user_context = nullptr;
};
struct OutputTarget {  // ACameraOutputTarget
    const void* window;
};
struct SessionOutput {  // ACaptureSessionOutput
    const void* window;
};
struct OutputContainer {  // ACaptureSessionOutputContainer
    std::vector<const void*> windows;
};

// Whether a window is one the camera can fill: a reader's, or the runtime's native window.
bool deliverable(const void* window) { return window == native_window_handle() || reader_of(window) != nullptr; }

GUEST int ACameraOutputTarget_create(void* window, OutputTarget** out) {
    if (!window || !out) return kErrInvalidParameter;
    *out = new OutputTarget{window};
    return kOk;
}
GUEST void ACameraOutputTarget_free(OutputTarget* t) { delete t; }
GUEST int ACaptureRequest_addTarget(Request* r, const OutputTarget* t) {
    if (!r || !t) return kErrInvalidParameter;
    if (std::find(r->targets.begin(), r->targets.end(), t->window) == r->targets.end()) r->targets.push_back(t->window);
    return kOk;
}
GUEST int ACaptureRequest_removeTarget(Request* r, const OutputTarget* t) {
    if (!r || !t) return kErrInvalidParameter;
    r->targets.erase(std::remove(r->targets.begin(), r->targets.end(), t->window), r->targets.end());
    return kOk;
}
GUEST int ACaptureRequest_getConstEntry(const Request* r, uint32_t tag, ConstEntry* out) {
    if (!r || !out) return kErrInvalidParameter;
    return r->settings.get(tag, out);
}
GUEST int ACaptureRequest_getAllTags(const Request* r, int32_t* n, const uint32_t** tags) {
    if (!r || !n || !tags) return kErrInvalidParameter;
    const auto& all = const_cast<Request*>(r)->settings.all_tags();
    *n = static_cast<int32_t>(all.size());
    *tags = all.data();
    return kOk;
}
#define WINAPK_SET_ENTRY(Name, ctype, type)                                                         \
    GUEST int ACaptureRequest_setEntry_##Name(Request* r, uint32_t tag, uint32_t count, const ctype* data) { \
        if (!r || (count && !data)) return kErrInvalidParameter;                                    \
        r->settings.set(tag, type, count, data);                                                    \
        return kOk;                                                                                 \
    }
WINAPK_SET_ENTRY(u8, uint8_t, kByte)
WINAPK_SET_ENTRY(i32, int32_t, kInt32)
WINAPK_SET_ENTRY(float, float, kFloat)
WINAPK_SET_ENTRY(i64, int64_t, kInt64)
WINAPK_SET_ENTRY(double, double, kDouble)
WINAPK_SET_ENTRY(rational, int64_t, kRational)  // ACameraMetadata_rational: two int32, 8 bytes
#undef WINAPK_SET_ENTRY
GUEST void ACaptureRequest_free(Request* r) { delete r; }
GUEST int ACaptureRequest_setUserContext(Request* r, void* context) {
    if (!r) return kErrInvalidParameter;
    r->user_context = context;
    return kOk;
}
GUEST int ACaptureRequest_getUserContext(const Request* r, void** context) {
    if (!r || !context) return kErrInvalidParameter;
    *context = r->user_context;
    return kOk;
}
GUEST Request* ACaptureRequest_copy(const Request* r) { return r ? new Request(*r) : nullptr; }

GUEST int ACaptureSessionOutputContainer_create(OutputContainer** out) {
    if (!out) return kErrInvalidParameter;
    *out = new OutputContainer;
    return kOk;
}
GUEST void ACaptureSessionOutputContainer_free(OutputContainer* c) { delete c; }
GUEST int ACaptureSessionOutput_create(void* window, SessionOutput** out) {
    if (!window || !out) return kErrInvalidParameter;
    *out = new SessionOutput{window};
    return kOk;
}
GUEST void ACaptureSessionOutput_free(SessionOutput* o) { delete o; }
GUEST int ACaptureSessionOutputContainer_add(OutputContainer* c, const SessionOutput* o) {
    if (!c || !o) return kErrInvalidParameter;
    if (std::find(c->windows.begin(), c->windows.end(), o->window) == c->windows.end()) c->windows.push_back(o->window);
    return kOk;
}
GUEST int ACaptureSessionOutputContainer_remove(OutputContainer* c, const SessionOutput* o) {
    if (!c || !o) return kErrInvalidParameter;
    c->windows.erase(std::remove(c->windows.begin(), c->windows.end(), o->window), c->windows.end());
    return kOk;
}

// ---- the characteristics ------------------------------------------------------------------------

std::shared_ptr<Metadata> characteristics(const camera::Info& info) {
    auto m = std::make_shared<Metadata>();
    m->u8(kLensFacing, {static_cast<uint8_t>(info.back ? 1 : 0)});  // BACK 1, FRONT 0
    m->i32(kSensorOrientation, {0});
    m->u8(kHardwareLevel, {2});  // LEGACY
    m->u8(kAvailableCapabilities, {0});  // BACKWARD_COMPATIBLE
    m->u8(kFlashInfoAvailable, {static_cast<uint8_t>(info.torch ? 1 : 0)});
    if (info.autofocus) {
        m->u8(kAfAvailableModes, {0, 1, 3, 4});  // OFF, AUTO, CONTINUOUS_VIDEO, CONTINUOUS_PICTURE
    } else {
        m->u8(kAfAvailableModes, {0});
        m->f32(kLensMinimumFocusDistance, {0.0f});
    }
    m->u8(kAeAvailableModes, {1});           // ON
    m->u8(kAvailableModes, {1});             // AUTO
    m->u8(kAwbAvailableModes, {1});          // AUTO
    m->u8(kAeAvailableAntibanding, {3});     // AUTO
    m->u8(kAeLockAvailable, {0});
    m->u8(kAwbLockAvailable, {0});
    m->i32(kMaxRegions, {0, 0, 0});
    m->i32(kAeCompensationRange, {0, 0});
    m->rational(kAeCompensationStep, 0, 1);
    m->u8(kAvailableFaceDetectModes, {0});
    m->f32(kMaxDigitalZoom, {1.0f});
    m->u8(kCroppingType, {0});  // CENTER_ONLY
    m->u8(kTimestampSource, {0});  // UNKNOWN
    m->i32(kMaxNumOutputStreams, {0, 3, 1});  // RAW, processed, stalling (JPEG)
    m->u8(kPipelineMaxDepth, {4});
    m->i32(kPartialResultCount, {1});
    m->i32(kSyncMaxLatency, {-1});  // UNKNOWN
    m->i32(kJpegAvailableThumbnailSizes, {0, 0, 160, 120, 320, 240});
    // Sizes and rates as CameraBackend.formats gives them: integer ranges rounded inwards, each once.
    std::vector<int32_t> configs, ranges;
    std::vector<int64_t> durations, stalls;
    int max_w = 0, max_h = 0;
    for (const camera::Size& s : info.sizes) {
        for (int32_t f : {kYuv420888, kJpeg, kPrivate}) {
            configs.insert(configs.end(), {f, s.width, s.height, 0});  // OUTPUT
            const int64_t frame = static_cast<int64_t>(1e9 / std::max(1.0, s.max_fps));
            durations.insert(durations.end(), {f, s.width, s.height, frame});
            stalls.insert(stalls.end(), {f, s.width, s.height, f == kJpeg ? frame : 0});
        }
        if (static_cast<int64_t>(s.width) * s.height > static_cast<int64_t>(max_w) * max_h) max_w = s.width, max_h = s.height;
        int lo = std::max(1, static_cast<int>(std::ceil(s.min_fps))), hi = static_cast<int>(std::floor(s.max_fps));
        if (hi < lo) hi = lo;
        bool seen = false;
        for (size_t i = 0; i < ranges.size(); i += 2) seen = seen || (ranges[i] == lo && ranges[i + 1] == hi);
        if (!seen) ranges.insert(ranges.end(), {lo, hi});
    }
    m->i32(kStreamConfigurations, configs);
    m->i64(kMinFrameDurations, durations);
    m->i64(kStallDurations, stalls);
    m->i32(kAeAvailableFpsRanges, ranges);
    if (max_w > 0) {
        m->i32(kActiveArraySize, {0, 0, max_w, max_h});
        m->i32(kPreCorrectionActiveArraySize, {0, 0, max_w, max_h});
        m->i32(kPixelArraySize, {max_w, max_h});
    }
    std::vector<int32_t> request_keys = {kControlMode, kAeMode, kAwbMode, kAfMode, kAeTargetFpsRange,
                                         kCaptureIntent, kJpegQuality, kJpegOrientation, kJpegThumbnailSize};
    if (info.torch) request_keys.push_back(kFlashMode);
    if (info.autofocus) request_keys.push_back(kAfTrigger);
    m->i32(kAvailableRequestKeys, request_keys);
    std::vector<int32_t> result_keys = request_keys;
    result_keys.insert(result_keys.end(), {kSensorTimestamp, kAfState, kAeState, kAwbState, kFlashState, kLensState});
    m->i32(kAvailableResultKeys, result_keys);
    std::vector<int32_t> char_keys;
    for (const auto& [tag, e] : m->entries) char_keys.push_back(static_cast<int32_t>(tag));
    char_keys.push_back(kAvailableCharacteristicsKeys);
    m->i32(kAvailableCharacteristicsKeys, char_keys);
    return m;
}

GUEST int ACameraMetadata_getConstEntry(const Metadata* m, uint32_t tag, ConstEntry* out) {
    if (!m || !out) return kErrInvalidParameter;
    return m->get(tag, out);
}
GUEST int ACameraMetadata_getAllTags(const Metadata* m, int32_t* n, const uint32_t** tags) {
    if (!m || !n || !tags) return kErrInvalidParameter;
    const auto& all = const_cast<Metadata*>(m)->all_tags();
    *n = static_cast<int32_t>(all.size());
    *tags = all.data();
    return kOk;
}
GUEST Metadata* ACameraMetadata_copy(const Metadata* m) { return m ? new Metadata(*m) : nullptr; }
GUEST void ACameraMetadata_free(Metadata* m) { delete m; }
GUEST bool ACameraMetadata_isLogicalMultiCamera(const Metadata*, size_t* n, const char* const** ids) {
    if (n) *n = 0;
    if (ids) *ids = nullptr;
    return false;
}

// ---- ACameraManager ---------------------------------------------------------------------------------

struct IdList {  // ACameraIdList
    int numCameras;
    const char** cameraIds;
};
struct AvailabilityCallbacks {  // ACameraManager_AvailabilityCallbacks
    void* context;
    void(GUEST* onCameraAvailable)(void* context, const char* id);
    void(GUEST* onCameraUnavailable)(void* context, const char* id);
};
struct ExtendedAvailabilityCallbacks {
    AvailabilityCallbacks availability;
    void(GUEST* onAccessPrioritiesChanged)(void* context);
    void* reserved[4];
};

struct Manager {
    Dispatcher dispatcher;
};

GUEST Manager* ACameraManager_create() { return new Manager; }
GUEST void ACameraManager_delete(Manager* m) { delete m; }

int camera_index(const char* id) {
    if (!id || !*id) return -1;
    char* end = nullptr;
    const long v = std::strtol(id, &end, 10);
    return *end == '\0' && v >= 0 && v < camera::count() ? static_cast<int>(v) : -1;
}

GUEST int ACameraManager_getCameraIdList(Manager* m, IdList** out) {
    if (!m || !out) return kErrInvalidParameter;
    const int n = camera::count();
    auto* list = new IdList{n, new const char*[static_cast<size_t>(std::max(n, 1))]};
    for (int i = 0; i < n; ++i) list->cameraIds[i] = _strdup(std::to_string(i).c_str());
    *out = list;
    return kOk;
}
GUEST void ACameraManager_deleteCameraIdList(IdList* list) {
    if (!list) return;
    for (int i = 0; i < list->numCameras; ++i) std::free(const_cast<char*>(list->cameraIds[i]));
    delete[] list->cameraIds;
    delete list;
}
// Every camera is available from the start, and never goes away while the app runs: a device
// tells a new listener of each camera at once, on its own thread.
GUEST int ACameraManager_registerAvailabilityCallback(Manager* m, const AvailabilityCallbacks* cb) {
    if (!m || !cb || !cb->onCameraAvailable) return kErrInvalidParameter;
    auto available = guest_fn(cb->onCameraAvailable);
    void* context = cb->context;
    const int n = camera::count();
    m->dispatcher.post([available, context, n] {
        for (int i = 0; i < n; ++i) available(context, std::to_string(i).c_str());
    });
    return kOk;
}
GUEST int ACameraManager_unregisterAvailabilityCallback(Manager* m, const AvailabilityCallbacks* cb) {
    return m && cb ? kOk : kErrInvalidParameter;
}
GUEST int ACameraManager_registerExtendedAvailabilityCallback(Manager* m, const ExtendedAvailabilityCallbacks* cb) {
    return cb ? ACameraManager_registerAvailabilityCallback(m, &cb->availability) : kErrInvalidParameter;
}
GUEST int ACameraManager_unregisterExtendedAvailabilityCallback(Manager* m, const ExtendedAvailabilityCallbacks* cb) {
    return m && cb ? kOk : kErrInvalidParameter;
}
GUEST int ACameraManager_getCameraCharacteristics(Manager* m, const char* id, Metadata** out) {
    if (!m || !out) return kErrInvalidParameter;
    const int index = camera_index(id);
    camera::Info info;
    if (index < 0 || !camera::info(index, &info)) return kErrInvalidParameter;
    *out = new Metadata(*characteristics(info));
    return kOk;
}
GUEST int ACameraManager_getTagFromName(Manager*, const char*, const char*, uint32_t*) {
    return kErrInvalidParameter;  // no vendor tags
}

// ---- ACameraDevice, ACameraCaptureSession ------------------------------------------------------------

struct DeviceCallbacks {  // ACameraDevice_StateCallbacks
    void* context;
    void(GUEST* onDisconnected)(void* context, void* device);
    void(GUEST* onError)(void* context, void* device, int error);
};
struct SessionCallbacks {  // ACameraCaptureSession_stateCallbacks
    void* context;
    void(GUEST* onClosed)(void* context, void* session);
    void(GUEST* onReady)(void* context, void* session);
    void(GUEST* onActive)(void* context, void* session);
};
struct CaptureFailure {  // ACameraCaptureFailure
    int64_t frameNumber;
    int reason;
    int sequenceId;
    bool wasImageCaptured;
};
// ACameraCaptureSession_captureCallbacks and its V2 (onCaptureStarted with the frame number).
struct CaptureCallbacks {
    void* context;
    void* onCaptureStarted;
    void(GUEST* onCaptureProgressed)(void*, void*, Request*, const Metadata*);
    void(GUEST* onCaptureCompleted)(void*, void*, Request*, const Metadata*);
    void(GUEST* onCaptureFailed)(void*, void*, Request*, CaptureFailure*);
    void(GUEST* onCaptureSequenceCompleted)(void*, void*, int sequence, int64_t frame);
    void(GUEST* onCaptureSequenceAborted)(void*, void*, int sequence);
    void(GUEST* onCaptureBufferLost)(void*, void*, Request*, void* window, int64_t frame);
};
using StartedV1 = void(GUEST*)(void*, void*, const Request*, int64_t timestamp);
using StartedV2 = void(GUEST*)(void*, void*, const Request*, int64_t timestamp, int64_t frame);

struct Callbacks {  // a sequence's, guest-callable
    CaptureCallbacks cb{};
    bool v2 = false;
};
Callbacks wrap(const CaptureCallbacks* in, bool v2) {
    Callbacks c;
    if (!in) return c;
    c.v2 = v2;
    c.cb.context = in->context;
    c.cb.onCaptureStarted = in->onCaptureStarted ? arm64::host_callable(in->onCaptureStarted) : nullptr;
    c.cb.onCaptureProgressed = guest_fn(in->onCaptureProgressed);
    c.cb.onCaptureCompleted = guest_fn(in->onCaptureCompleted);
    c.cb.onCaptureFailed = guest_fn(in->onCaptureFailed);
    c.cb.onCaptureSequenceCompleted = guest_fn(in->onCaptureSequenceCompleted);
    c.cb.onCaptureSequenceAborted = guest_fn(in->onCaptureSequenceAborted);
    c.cb.onCaptureBufferLost = guest_fn(in->onCaptureBufferLost);
    return c;
}

struct Device;
struct CaptureSession {
    Device* device = nullptr;
    std::vector<const void*> outputs;
    SessionCallbacks callbacks{};
    bool closed = false;
    bool active = false;
    // The repeating sequence: its requests (copies) in turn, its callbacks.
    std::vector<std::shared_ptr<Request>> repeating;
    size_t repeating_next = 0;
    Callbacks repeating_callbacks;
    int repeating_sequence = -1;
    int64_t repeating_last_frame = -1;
    // One-shot captures, in order; the last of a sequence ends it.
    struct Shot {
        std::shared_ptr<Request> request;
        Callbacks callbacks;
        int sequence;
        bool last;
    };
    std::deque<Shot> shots;
};

struct Device {
    int index = 0;
    std::string id;
    camera::Info info;
    DeviceCallbacks callbacks{};
    std::shared_ptr<camera::Stream> stream;
    std::unique_ptr<Dispatcher> dispatcher = std::make_unique<Dispatcher>();
    std::mutex lock;
    std::shared_ptr<CaptureSession> session;
    int next_sequence = 0;
    int64_t frame_number = 0;
    bool streaming = false, starting = false;
    int stream_w = 0, stream_h = 0, stream_fps = 0;
    bool focus_continuous = false, torch = false;
    // The newest picture not yet handled (latest wins, as a camera drops what nobody takes).
    std::mutex frame_lock;
    camera::Picture frame;
    bool frame_pending = false;
};

// Under the device's lock: whether the session has work, and the state callbacks that follow.
void update_activity(Device* d, CaptureSession* s) {
    const bool busy = !s->closed && (!s->repeating.empty() || !s->shots.empty());
    if (busy == s->active) return;
    s->active = busy;
    const SessionCallbacks cb = s->callbacks;
    auto fn = busy ? cb.onActive : cb.onReady;
    if (fn) d->dispatcher->post([fn, ctx = cb.context, s] { fn(ctx, s); });
}

// Under the device's lock: the size the webcam captures at for the session's outputs (the largest
// reader's, else the size closest to 1280x720 for the window alone), and its rate.
void plan_stream(Device* d, CaptureSession* s, int fps) {
    int w = 0, h = 0;
    for (const void* win : s->outputs) {
        if (auto r = reader_of(win)) {
            if (static_cast<int64_t>(r->width) * r->height > static_cast<int64_t>(w) * h) w = r->width, h = r->height;
        }
    }
    if (w == 0) w = 1280, h = 720;
    d->stream_w = w, d->stream_h = h, d->stream_fps = fps > 0 ? fps : 30;
}

void handle_frame(const std::shared_ptr<Device>& d);

// On the dispatcher: (re)starts the webcam at the planned size. Opening it takes a while.
void start_stream(const std::shared_ptr<Device>& d) {
    int w, h, fps;
    {
        std::lock_guard<std::mutex> hold(d->lock);
        if (!d->session || d->session->closed) {
            d->starting = false;
            return;
        }
        w = d->stream_w, h = d->stream_h, fps = d->stream_fps;
    }
    std::weak_ptr<Device> weak = d;
    d->stream->set_listener([weak](const camera::Picture& p) {
        auto dev = weak.lock();
        if (!dev) return;
        bool post;
        {
            std::lock_guard<std::mutex> hold(dev->frame_lock);
            post = !dev->frame_pending;
            dev->frame = p;
            dev->frame_pending = true;
        }
        if (post) dev->dispatcher->post([weak] {
            if (auto dev2 = weak.lock()) handle_frame(dev2);
        });
    });
    const bool ok = d->stream->configure(w, h, fps) && d->stream->start();
    std::lock_guard<std::mutex> hold(d->lock);
    d->starting = false;
    d->streaming = ok;
    if (!ok) {
        log_once("the webcam would not start for camera", std::to_string(d->index));
        if (d->callbacks.onError) {
            auto fn = d->callbacks.onError;
            void* ctx = d->callbacks.context;
            Device* raw = d.get();
            d->dispatcher->post([fn, ctx, raw] { fn(ctx, raw, 4); });  // ERROR_CAMERA_DEVICE
        }
    }
}

// Under the device's lock: makes sure the webcam runs at the session's plan.
void ensure_stream(const std::shared_ptr<Device>& d, int fps) {
    const int w = d->stream_w, h = d->stream_h, was_fps = d->stream_fps;
    plan_stream(d.get(), d->session.get(), fps);
    if (d->starting || (d->streaming && w == d->stream_w && h == d->stream_h && was_fps == d->stream_fps)) return;
    d->starting = true;
    d->dispatcher->post([d] { start_stream(d); });
}

// On the dispatcher: one picture serves the next request.
void handle_frame(const std::shared_ptr<Device>& d) {
    camera::Picture pic;
    {
        std::lock_guard<std::mutex> hold(d->frame_lock);
        if (!d->frame_pending) return;
        pic = d->frame;
        d->frame = {};
        d->frame_pending = false;
    }
    std::shared_ptr<CaptureSession> s;
    std::shared_ptr<Request> request;
    Callbacks cb;
    int sequence = -1;
    bool ends_sequence = false, from_shot = false, set_focus = false, set_torch = false, focus = false, torch = false;
    int64_t frame;
    {
        std::lock_guard<std::mutex> hold(d->lock);
        s = d->session;
        if (!s || s->closed) return;
        if (!s->shots.empty()) {
            CaptureSession::Shot shot = std::move(s->shots.front());
            s->shots.pop_front();
            request = shot.request, cb = shot.callbacks, sequence = shot.sequence, ends_sequence = shot.last;
            from_shot = true;
        } else if (!s->repeating.empty()) {
            request = s->repeating[s->repeating_next++ % s->repeating.size()];
            cb = s->repeating_callbacks;
            sequence = s->repeating_sequence;
        } else {
            return;
        }
        frame = d->frame_number++;
        if (!from_shot) s->repeating_last_frame = frame;
        // The controls the webcam has, autofocus and torch, as the request says (set below, without
        // the lock: the torch waits for the capture thread).
        const int af = request->settings.byte_or(kAfMode, d->focus_continuous ? 4 : 0);
        focus = af == 3 || af == 4;
        set_focus = focus != d->focus_continuous && d->info.autofocus;
        if (set_focus) d->focus_continuous = focus;
        torch = request->settings.byte_or(kFlashMode, 0) == 2;
        set_torch = torch != d->torch && d->info.torch;
        if (set_torch) d->torch = torch;
    }
    if (set_focus) d->stream->set_continuous_focus(focus);
    if (set_torch && !d->stream->set_torch(torch) && torch) {
        std::lock_guard<std::mutex> hold(d->lock);
        d->torch = false;
    }
    // The targets.
    const int quality = request->settings.byte_or(kJpegQuality, 95);
    for (const void* win : request->targets) {
        if (std::find(s->outputs.begin(), s->outputs.end(), win) == s->outputs.end()) continue;
        if (win == native_window_handle()) {
            std::vector<uint8_t> nv12 = scale_nv12(pic, pic.width, pic.height);
            std::vector<uint32_t> argb(static_cast<size_t>(pic.width) * pic.height);
            nv12_to_rgb(nv12.data(), pic.width, pic.height, [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
                argb[static_cast<size_t>(pic.width) * y + x] = 0xff000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
            });
            guest_frame_present(argb.data(), pic.width, pic.height);
            continue;
        }
        auto r = reader_of(win);
        if (!r) continue;
        auto img = make_image(pic, r->format, r->width, r->height, quality);
        if (!img) continue;
        ImageListener listener;
        {
            std::lock_guard<std::mutex> hold(r->lock);
            // A full queue drops its oldest, as a camera stream drops frames nobody takes.
            while (!r->queue.empty() && static_cast<int>(r->queue.size()) + r->acquired >= r->max_images) {
                r->queue.pop_front();
            }
            if (static_cast<int>(r->queue.size()) + r->acquired >= r->max_images) continue;
            r->queue.push_back(img);
            listener = r->listener;
        }
        if (listener.onImageAvailable) listener.onImageAvailable(listener.context, r.get());
    }
    // The callbacks, with the result.
    if (cb.cb.onCaptureStarted) {
        if (cb.v2) reinterpret_cast<StartedV2>(cb.cb.onCaptureStarted)(cb.cb.context, s.get(), request.get(), pic.timestamp_ns, frame);
        else reinterpret_cast<StartedV1>(cb.cb.onCaptureStarted)(cb.cb.context, s.get(), request.get(), pic.timestamp_ns);
    }
    if (cb.cb.onCaptureCompleted) {
        Metadata result = request->settings;
        result.i64(kSensorTimestamp, {pic.timestamp_ns});
        result.i64(kSensorFrameDuration, {1000000000LL / std::max(1, d->stream_fps)});
        const int focus = d->stream->focus_state();
        const int af_mode = request->settings.byte_or(kAfMode, 0);
        uint8_t af_state = 0;  // INACTIVE
        if (af_mode == 3 || af_mode == 4) af_state = focus == 1 ? 1 : focus == 2 ? 2 : 0;  // PASSIVE_SCAN, PASSIVE_FOCUSED
        result.u8(kAfState, {af_state});
        result.u8(kAeState, {2});    // CONVERGED
        result.u8(kAwbState, {2});   // CONVERGED
        result.u8(kFlashState, {static_cast<uint8_t>(d->info.torch ? (d->torch ? 3 : 2) : 0)});  // FIRED/READY, UNAVAILABLE
        result.u8(kLensState, {static_cast<uint8_t>(focus == 1 ? 1 : 0)});
        cb.cb.onCaptureCompleted(cb.cb.context, s.get(), request.get(), &result);
    }
    if (ends_sequence && cb.cb.onCaptureSequenceCompleted) {
        cb.cb.onCaptureSequenceCompleted(cb.cb.context, s.get(), sequence, frame);
    }
    if (from_shot) {
        std::lock_guard<std::mutex> hold(d->lock);
        if (d->dispatcher) update_activity(d.get(), s.get());
    }
}

std::mutex g_devices_lock;
std::map<const void*, std::shared_ptr<Device>> g_devices;  // by ACameraDevice*, and by session

std::shared_ptr<Device> device_of(const void* p) {
    std::lock_guard<std::mutex> hold(g_devices_lock);
    auto it = g_devices.find(p);
    return it == g_devices.end() ? nullptr : it->second;
}

GUEST int ACameraManager_openCamera(Manager* m, const char* id, DeviceCallbacks* callbacks, Device** out) {
    if (!m || !callbacks || !out) return kErrInvalidParameter;
    const int index = camera_index(id);
    if (index < 0) return kErrInvalidParameter;
    if (!camera::permitted()) return kErrPermissionDenied;
    if (!camera::allowed()) return kErrCameraDisabled;
    auto d = std::make_shared<Device>();
    d->index = index;
    d->id = std::to_string(index);
    if (!camera::info(index, &d->info) || !(d->stream = camera::open(index))) return kErrCameraDisabled;
    d->callbacks = {callbacks->context, guest_fn(callbacks->onDisconnected), guest_fn(callbacks->onError)};
    {
        std::lock_guard<std::mutex> hold(g_devices_lock);
        g_devices[d.get()] = d;
    }
    *out = d.get();
    return kOk;
}

// Under the device's lock: the session is closed, its sequences aborted, onClosed posted.
void close_session(Device* d, const std::shared_ptr<CaptureSession>& s) {
    if (!s || s->closed) return;
    s->closed = true;
    if (!s->repeating.empty() && s->repeating_callbacks.cb.onCaptureSequenceAborted) {
        auto fn = s->repeating_callbacks.cb.onCaptureSequenceAborted;
        d->dispatcher->post([fn, ctx = s->repeating_callbacks.cb.context, s, seq = s->repeating_sequence] {
            fn(ctx, s.get(), seq);
        });
    }
    for (const auto& shot : s->shots) {
        if (shot.last && shot.callbacks.cb.onCaptureSequenceAborted) {
            auto fn = shot.callbacks.cb.onCaptureSequenceAborted;
            d->dispatcher->post([fn, ctx = shot.callbacks.cb.context, s, seq = shot.sequence] { fn(ctx, s.get(), seq); });
        }
    }
    s->repeating.clear();
    s->shots.clear();
    if (s->callbacks.onClosed) {
        auto fn = s->callbacks.onClosed;
        d->dispatcher->post([fn, ctx = s->callbacks.context, s] { fn(ctx, s.get()); });
    }
}

GUEST int ACameraDevice_close(Device* raw) {
    auto d = device_of(raw);
    if (!d) return kErrInvalidParameter;
    {
        std::lock_guard<std::mutex> hold(d->lock);
        close_session(d.get(), d->session);
        if (d->session) {
            std::lock_guard<std::mutex> g(g_devices_lock);
            g_devices.erase(d->session.get());
        }
        d->session.reset();
    }
    {
        std::lock_guard<std::mutex> hold(g_devices_lock);
        g_devices.erase(raw);
    }
    d->stream->set_listener(nullptr);
    d->stream->stop();
    // The dispatcher runs what was posted (onClosed) and ends; from one of its own callbacks it is
    // let go of rather than joined.
    if (d->dispatcher->on_thread()) d->dispatcher.release()->finish();
    else d->dispatcher.reset();
    return kOk;
}
GUEST const char* ACameraDevice_getId(const Device* raw) { return raw ? raw->id.c_str() : nullptr; }

GUEST int ACameraDevice_createCaptureRequest(const Device* raw, int template_id, Request** out) {
    auto d = device_of(raw);
    if (!d || !out) return kErrInvalidParameter;
    if (template_id < 1 || template_id > 6) return kErrInvalidParameter;
    if (template_id == 5 || template_id == 6) return kErrInvalidParameter;  // ZSL, MANUAL: not at LEGACY
    auto* r = new Request;
    Metadata& m = r->settings;
    m.u8(kControlMode, {1});
    m.u8(kAeMode, {1});
    m.u8(kAwbMode, {1});
    m.u8(kAeAntibandingMode, {3});
    m.u8(kAfMode, {static_cast<uint8_t>(d->info.autofocus ? (template_id == 3 ? 3 : 4) : 0)});
    m.u8(kCaptureIntent, {static_cast<uint8_t>(template_id)});
    m.u8(kFlashMode, {0});
    m.u8(kJpegQuality, {95});
    m.i32(kJpegOrientation, {0});
    m.i32(kJpegThumbnailSize, {160, 120});
    m.u8(kFaceDetectMode, {0});
    int lo = 15, hi = 30;
    if (!d->info.sizes.empty()) hi = static_cast<int>(d->info.sizes.front().max_fps), lo = template_id == 3 ? hi : std::min(lo, hi);
    m.i32(kAeTargetFpsRange, {lo, hi});
    *out = r;
    return kOk;
}
GUEST int ACameraDevice_createCaptureRequest_withPhysicalIds(const Device* raw, int template_id, const IdList* ids,
                                                             Request** out) {
    if (ids && ids->numCameras > 0) return kErrInvalidParameter;
    return ACameraDevice_createCaptureRequest(raw, template_id, out);
}

int create_session(Device* raw, const OutputContainer* outputs, const SessionCallbacks* callbacks, void** out) {
    auto d = device_of(raw);
    if (!d || !outputs || !callbacks || !out) return kErrInvalidParameter;
    if (outputs->windows.empty()) return kErrInvalidParameter;
    for (const void* w : outputs->windows) {
        if (!deliverable(w)) {
            log_once("a session output that is neither an AImageReader's window nor the app's native window;"
                     " it will get no pictures", "");
        }
    }
    auto s = std::make_shared<CaptureSession>();
    s->device = raw;
    s->outputs = outputs->windows;
    s->callbacks = {callbacks->context, guest_fn(callbacks->onClosed), guest_fn(callbacks->onReady),
                    guest_fn(callbacks->onActive)};
    {
        std::lock_guard<std::mutex> hold(d->lock);
        if (d->session) {
            close_session(d.get(), d->session);
            std::lock_guard<std::mutex> g(g_devices_lock);
            g_devices.erase(d->session.get());
        }
        d->session = s;
        plan_stream(d.get(), s.get(), d->stream_fps);
        // Configured: ready, as a device's session is once its streams are.
        if (s->callbacks.onReady) {
            auto fn = s->callbacks.onReady;
            d->dispatcher->post([fn, ctx = s->callbacks.context, s] { fn(ctx, s.get()); });
        }
    }
    {
        std::lock_guard<std::mutex> hold(g_devices_lock);
        g_devices[s.get()] = d;
    }
    *out = s.get();
    return kOk;
}
GUEST int ACameraDevice_createCaptureSession(Device* raw, const OutputContainer* outputs,
                                             const SessionCallbacks* callbacks, void** out) {
    return create_session(raw, outputs, callbacks, out);
}
GUEST int ACameraDevice_createCaptureSessionWithSessionParameters(Device* raw, const OutputContainer* outputs,
                                                                  const Request*, const SessionCallbacks* callbacks,
                                                                  void** out) {
    return create_session(raw, outputs, callbacks, out);
}
GUEST int ACameraDevice_isSessionConfigurationSupported(const Device* raw, const OutputContainer* outputs) {
    if (!device_of(raw) || !outputs) return kErrInvalidParameter;
    for (const void* w : outputs->windows) {
        if (!deliverable(w)) return kErrStreamConfigureFail;
    }
    return kOk;
}

// The session's device, its session object checked.
std::shared_ptr<Device> live(void* session, std::shared_ptr<CaptureSession>* out) {
    auto d = device_of(session);
    if (!d) return nullptr;
    std::lock_guard<std::mutex> hold(d->lock);
    if (!d->session || d->session.get() != session) return nullptr;
    *out = d->session;
    return d;
}

int submit(void* session, const CaptureCallbacks* callbacks, bool v2, int n, Request** requests, int* sequence_out,
           bool repeating) {
    if (!session || n < 1 || !requests) return kErrInvalidParameter;
    std::shared_ptr<CaptureSession> s;
    auto d = live(session, &s);
    if (!d) return kErrSessionClosed;
    for (int i = 0; i < n; ++i) {
        if (!requests[i] || requests[i]->targets.empty()) return kErrInvalidParameter;
        for (const void* t : requests[i]->targets) {
            if (std::find(s->outputs.begin(), s->outputs.end(), t) == s->outputs.end()) return kErrInvalidParameter;
        }
    }
    std::lock_guard<std::mutex> hold(d->lock);
    if (s->closed) return kErrSessionClosed;
    const int sequence = d->next_sequence++;
    const Callbacks cb = wrap(callbacks, v2);
    if (repeating) {
        // A new repeating sequence replaces the old, which ends at the last frame it got.
        if (!s->repeating.empty() && s->repeating_callbacks.cb.onCaptureSequenceCompleted) {
            auto fn = s->repeating_callbacks.cb.onCaptureSequenceCompleted;
            d->dispatcher->post([fn, ctx = s->repeating_callbacks.cb.context, s, seq = s->repeating_sequence,
                                 last = s->repeating_last_frame] { fn(ctx, s.get(), seq, last); });
        }
        s->repeating.clear();
        for (int i = 0; i < n; ++i) s->repeating.push_back(std::make_shared<Request>(*requests[i]));
        s->repeating_next = 0;
        s->repeating_callbacks = cb;
        s->repeating_sequence = sequence;
        s->repeating_last_frame = -1;
    } else {
        for (int i = 0; i < n; ++i) {
            s->shots.push_back({std::make_shared<Request>(*requests[i]), cb, sequence, i == n - 1});
        }
    }
    int32_t fps = requests[0]->settings.int32_or(kAeTargetFpsRange, 30, 1);
    ensure_stream(d, fps);
    update_activity(d.get(), s.get());
    if (sequence_out) *sequence_out = sequence;
    return kOk;
}
GUEST int ACameraCaptureSession_capture(void* s, const CaptureCallbacks* cb, int n, Request** r, int* seq) {
    return submit(s, cb, false, n, r, seq, false);
}
GUEST int ACameraCaptureSession_setRepeatingRequest(void* s, const CaptureCallbacks* cb, int n, Request** r, int* seq) {
    return submit(s, cb, false, n, r, seq, true);
}
GUEST int ACameraCaptureSession_captureV2(void* s, const CaptureCallbacks* cb, int n, Request** r, int* seq) {
    return submit(s, cb, true, n, r, seq, false);
}
GUEST int ACameraCaptureSession_setRepeatingRequestV2(void* s, const CaptureCallbacks* cb, int n, Request** r,
                                                      int* seq) {
    return submit(s, cb, true, n, r, seq, true);
}
GUEST int ACameraCaptureSession_stopRepeating(void* session) {
    std::shared_ptr<CaptureSession> s;
    auto d = live(session, &s);
    if (!d) return kErrSessionClosed;
    std::lock_guard<std::mutex> hold(d->lock);
    if (!s->repeating.empty()) {
        if (s->repeating_callbacks.cb.onCaptureSequenceCompleted) {
            auto fn = s->repeating_callbacks.cb.onCaptureSequenceCompleted;
            d->dispatcher->post([fn, ctx = s->repeating_callbacks.cb.context, s, seq = s->repeating_sequence,
                                 last = s->repeating_last_frame] { fn(ctx, s.get(), seq, last); });
        }
        s->repeating.clear();
    }
    update_activity(d.get(), s.get());
    return kOk;
}
GUEST int ACameraCaptureSession_abortCaptures(void* session) {
    std::shared_ptr<CaptureSession> s;
    auto d = live(session, &s);
    if (!d) return kErrSessionClosed;
    std::lock_guard<std::mutex> hold(d->lock);
    for (const auto& shot : s->shots) {
        if (shot.last && shot.callbacks.cb.onCaptureSequenceAborted) {
            auto fn = shot.callbacks.cb.onCaptureSequenceAborted;
            d->dispatcher->post([fn, ctx = shot.callbacks.cb.context, s, seq = shot.sequence] { fn(ctx, s.get(), seq); });
        }
    }
    s->shots.clear();
    if (!s->repeating.empty() && s->repeating_callbacks.cb.onCaptureSequenceAborted) {
        auto fn = s->repeating_callbacks.cb.onCaptureSequenceAborted;
        d->dispatcher->post([fn, ctx = s->repeating_callbacks.cb.context, s, seq = s->repeating_sequence] {
            fn(ctx, s.get(), seq);
        });
    }
    s->repeating.clear();
    update_activity(d.get(), s.get());
    return kOk;
}
GUEST void ACameraCaptureSession_close(void* session) {
    auto d = device_of(session);
    if (!d) return;
    std::lock_guard<std::mutex> hold(d->lock);
    if (!d->session || d->session.get() != session) return;
    close_session(d.get(), d->session);
    {
        std::lock_guard<std::mutex> g(g_devices_lock);
        g_devices.erase(session);
    }
    d->session.reset();
}
GUEST int ACameraCaptureSession_getDevice(void* session, void** device) {
    if (!session || !device) return kErrInvalidParameter;
    std::shared_ptr<CaptureSession> s;
    if (!live(session, &s)) return kErrSessionClosed;
    *device = s->device;
    return kOk;
}
GUEST int ACameraCaptureSession_updateSharedOutput(void*, void*) { return kErrUnsupportedOperation; }
GUEST int ACaptureSessionSharedOutput_create(void*, SessionOutput** out) {
    if (out) *out = nullptr;
    return kErrUnsupportedOperation;
}
GUEST int ACaptureSessionSharedOutput_add(void*, void*) { return kErrUnsupportedOperation; }
GUEST int ACaptureSessionSharedOutput_remove(void*, void*) { return kErrUnsupportedOperation; }
GUEST int ACaptureSessionPhysicalOutput_create(void*, const char*, SessionOutput** out) {
    if (out) *out = nullptr;
    return kErrUnsupportedOperation;
}
GUEST int ACameraCaptureSession_setWindowPreparedCallback(void* s, void*, void*) {
    return s ? kOk : kErrInvalidParameter;
}
GUEST int ACameraCaptureSession_prepareWindow(void* s, void*) { return s ? kOk : kErrInvalidParameter; }

struct Function {
    const char* name;
    void* fn;
};
#define WINAPK_NDK_CAMERA(fn) {#fn, reinterpret_cast<void*>(&fn)}
const Function kFunctions[] = {
    WINAPK_NDK_CAMERA(ACameraManager_create), WINAPK_NDK_CAMERA(ACameraManager_delete),
    WINAPK_NDK_CAMERA(ACameraManager_getCameraIdList), WINAPK_NDK_CAMERA(ACameraManager_deleteCameraIdList),
    WINAPK_NDK_CAMERA(ACameraManager_registerAvailabilityCallback),
    WINAPK_NDK_CAMERA(ACameraManager_unregisterAvailabilityCallback),
    WINAPK_NDK_CAMERA(ACameraManager_registerExtendedAvailabilityCallback),
    WINAPK_NDK_CAMERA(ACameraManager_unregisterExtendedAvailabilityCallback),
    WINAPK_NDK_CAMERA(ACameraManager_getCameraCharacteristics), WINAPK_NDK_CAMERA(ACameraManager_openCamera),
    WINAPK_NDK_CAMERA(ACameraManager_getTagFromName),
    WINAPK_NDK_CAMERA(ACameraMetadata_getConstEntry), WINAPK_NDK_CAMERA(ACameraMetadata_getAllTags),
    WINAPK_NDK_CAMERA(ACameraMetadata_copy), WINAPK_NDK_CAMERA(ACameraMetadata_free),
    WINAPK_NDK_CAMERA(ACameraMetadata_isLogicalMultiCamera),
    WINAPK_NDK_CAMERA(ACameraDevice_close), WINAPK_NDK_CAMERA(ACameraDevice_getId),
    WINAPK_NDK_CAMERA(ACameraDevice_createCaptureRequest),
    WINAPK_NDK_CAMERA(ACameraDevice_createCaptureRequest_withPhysicalIds),
    WINAPK_NDK_CAMERA(ACameraDevice_createCaptureSession),
    WINAPK_NDK_CAMERA(ACameraDevice_createCaptureSessionWithSessionParameters),
    WINAPK_NDK_CAMERA(ACameraDevice_isSessionConfigurationSupported),
    WINAPK_NDK_CAMERA(ACaptureSessionOutputContainer_create), WINAPK_NDK_CAMERA(ACaptureSessionOutputContainer_free),
    WINAPK_NDK_CAMERA(ACaptureSessionOutputContainer_add), WINAPK_NDK_CAMERA(ACaptureSessionOutputContainer_remove),
    WINAPK_NDK_CAMERA(ACaptureSessionOutput_create), WINAPK_NDK_CAMERA(ACaptureSessionOutput_free),
    WINAPK_NDK_CAMERA(ACaptureSessionSharedOutput_create), WINAPK_NDK_CAMERA(ACaptureSessionSharedOutput_add),
    WINAPK_NDK_CAMERA(ACaptureSessionSharedOutput_remove), WINAPK_NDK_CAMERA(ACaptureSessionPhysicalOutput_create),
    WINAPK_NDK_CAMERA(ACameraOutputTarget_create), WINAPK_NDK_CAMERA(ACameraOutputTarget_free),
    WINAPK_NDK_CAMERA(ACaptureRequest_addTarget), WINAPK_NDK_CAMERA(ACaptureRequest_removeTarget),
    WINAPK_NDK_CAMERA(ACaptureRequest_getConstEntry), WINAPK_NDK_CAMERA(ACaptureRequest_getAllTags),
    WINAPK_NDK_CAMERA(ACaptureRequest_setEntry_u8), WINAPK_NDK_CAMERA(ACaptureRequest_setEntry_i32),
    WINAPK_NDK_CAMERA(ACaptureRequest_setEntry_float), WINAPK_NDK_CAMERA(ACaptureRequest_setEntry_i64),
    WINAPK_NDK_CAMERA(ACaptureRequest_setEntry_double), WINAPK_NDK_CAMERA(ACaptureRequest_setEntry_rational),
    WINAPK_NDK_CAMERA(ACaptureRequest_free), WINAPK_NDK_CAMERA(ACaptureRequest_setUserContext),
    WINAPK_NDK_CAMERA(ACaptureRequest_getUserContext), WINAPK_NDK_CAMERA(ACaptureRequest_copy),
    WINAPK_NDK_CAMERA(ACameraCaptureSession_capture), WINAPK_NDK_CAMERA(ACameraCaptureSession_setRepeatingRequest),
    WINAPK_NDK_CAMERA(ACameraCaptureSession_captureV2),
    WINAPK_NDK_CAMERA(ACameraCaptureSession_setRepeatingRequestV2),
    WINAPK_NDK_CAMERA(ACameraCaptureSession_stopRepeating), WINAPK_NDK_CAMERA(ACameraCaptureSession_abortCaptures),
    WINAPK_NDK_CAMERA(ACameraCaptureSession_close), WINAPK_NDK_CAMERA(ACameraCaptureSession_getDevice),
    WINAPK_NDK_CAMERA(ACameraCaptureSession_updateSharedOutput),
    WINAPK_NDK_CAMERA(ACameraCaptureSession_setWindowPreparedCallback),
    WINAPK_NDK_CAMERA(ACameraCaptureSession_prepareWindow),
    WINAPK_NDK_CAMERA(AImageReader_new), WINAPK_NDK_CAMERA(AImageReader_newWithUsage),
    WINAPK_NDK_CAMERA(AImageReader_delete), WINAPK_NDK_CAMERA(AImageReader_getWindow),
    WINAPK_NDK_CAMERA(AImageReader_getWidth), WINAPK_NDK_CAMERA(AImageReader_getHeight),
    WINAPK_NDK_CAMERA(AImageReader_getFormat), WINAPK_NDK_CAMERA(AImageReader_getMaxImages),
    WINAPK_NDK_CAMERA(AImageReader_setImageListener), WINAPK_NDK_CAMERA(AImageReader_setBufferRemovedListener),
    WINAPK_NDK_CAMERA(AImageReader_acquireNextImage), WINAPK_NDK_CAMERA(AImageReader_acquireLatestImage),
    WINAPK_NDK_CAMERA(AImageReader_acquireNextImageAsync), WINAPK_NDK_CAMERA(AImageReader_acquireLatestImageAsync),
    WINAPK_NDK_CAMERA(AImage_delete), WINAPK_NDK_CAMERA(AImage_deleteAsync), WINAPK_NDK_CAMERA(AImage_getWidth),
    WINAPK_NDK_CAMERA(AImage_getHeight), WINAPK_NDK_CAMERA(AImage_getFormat), WINAPK_NDK_CAMERA(AImage_getCropRect),
    WINAPK_NDK_CAMERA(AImage_getTimestamp), WINAPK_NDK_CAMERA(AImage_getNumberOfPlanes),
    WINAPK_NDK_CAMERA(AImage_getPlanePixelStride), WINAPK_NDK_CAMERA(AImage_getPlaneRowStride),
    WINAPK_NDK_CAMERA(AImage_getPlaneData), WINAPK_NDK_CAMERA(AImage_getHardwareBuffer),
    WINAPK_NDK_CAMERA(AImage_getDataSpace),
};
#undef WINAPK_NDK_CAMERA

}  // namespace

void* ndk_camera_resolve(const char* name) {
    if (std::strncmp(name, "ACamera", 7) != 0 && std::strncmp(name, "ACapture", 8) != 0 &&
        std::strncmp(name, "AImage", 6) != 0) {
        return nullptr;
    }
    for (const Function& f : kFunctions) {
        if (std::strcmp(name, f.name) == 0) return f.fn;
    }
    return nullptr;
}

}  // namespace winapk::host
