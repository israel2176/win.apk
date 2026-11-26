// ndk_camera_test.cpp -- winapk-ndk-camera-test: libcamera2ndk and AImageReader
// (host/ndk_camera.cpp) as an app's native code calls them, System V, through their exported
// names, on a camera of the test's own (camera_host.h stood in for: one webcam, offered as a back
// and a front camera, 640x480 and 320x240 at up to 30 fps, whose frames are red in the top half
// and blue in the bottom). No webcam, JVM or window is needed, so it runs on CI.
//
// Checks: the id list and the characteristics (facing, LEGACY, the stream configurations with
// JPEG as AIMAGE_FORMAT_JPEG, the frame-rate range, autofocus modes, the keys lists); CAMERA
// refused is ACAMERA_ERROR_PERMISSION_DENIED; a preview request's template; a session on a
// YUV_420_888 reader, a JPEG reader and the native window: onReady, then a repeating request
// that starts the camera at the largest reader's size, turns on continuous autofocus, and gives
// onActive, images with the planes camera2 has (semiplanar, scaled to the reader, the right
// colours) whose timestamps are the results' SENSOR_TIMESTAMP, and pictures on the window; a
// one-shot JPEG capture (a whole JPEG, its sequence completed); the torch from FLASH_MODE;
// NO_BUFFER_AVAILABLE and MAX_IMAGES_ACQUIRED; stopRepeating (the sequence completed, onReady);
// closing the session (onClosed) and the device (the camera stopped).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "../host/camera_host.h"
#include "../host/ndk_camera.h"
#include "../host/native_window.h"
#include "winapk/guest_abi.h"

// ---- the stand-ins ---------------------------------------------------------------------------

namespace {
std::atomic<bool> g_permitted{true};
std::atomic<int> g_configured_w{0}, g_configured_h{0}, g_started{0}, g_stopped{0};
std::atomic<bool> g_focus_continuous{false}, g_torch{false}, g_torch_was_on{false};
int g_window = 0;
std::atomic<int> g_shown{0};
}  // namespace

namespace winapk {
namespace arm64 {
void* host_callable(void* fn) { return fn; }
}  // namespace arm64
namespace host {
const void* native_window_handle() { return &g_window; }
void guest_frame_present(const uint32_t* argb, int w, int h) {
    // Red over blue, as the camera sends it.
    const uint32_t top = argb[static_cast<size_t>(w) * 4 + 4], bottom = argb[static_cast<size_t>(w) * (h - 4) + 4];
    if (((top >> 16) & 0xff) > 180 && (bottom & 0xff) > 180) ++g_shown;
}

namespace camera {

class FakeStream : public Stream {
public:
    ~FakeStream() override { stop(); }
    bool configure(int w, int, int) override {
        w_ = w >= 600 ? 640 : 320, h_ = w >= 600 ? 480 : 240;  // the advertised size nearest
        g_configured_w = w_, g_configured_h = h_;
        return true;
    }
    int width() override { return w_; }
    int height() override { return h_; }
    bool start() override {
        if (thread_.joinable()) return true;
        ++g_started;
        stop_ = false;
        thread_ = std::thread([this] { run(); });
        return true;
    }
    void stop() override {
        if (!thread_.joinable()) return;
        stop_ = true;
        thread_.join();
        ++g_stopped;
    }
    void set_listener(std::function<void(const Picture&)> listener) override {
        std::lock_guard<std::mutex> hold(lock_);
        listener_ = std::move(listener);
    }
    void set_continuous_focus(bool on) override { g_focus_continuous = on; }
    bool set_torch(bool on) override {
        g_torch = on;
        if (on) g_torch_was_on = true;
        return true;
    }
    int focus_state() override { return g_focus_continuous ? 2 : 0; }

private:
    void run() {
        while (!stop_) {
            auto f = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(w_) * h_ * 3 / 2);
            uint8_t* y = f->data();
            uint8_t* uv = y + static_cast<size_t>(w_) * h_;
            for (int r = 0; r < h_; ++r) std::memset(y + static_cast<size_t>(w_) * r, r < h_ / 2 ? 81 : 41, w_);
            for (int r = 0; r < h_ / 2; ++r) {
                for (int x = 0; x < w_ / 2; ++x) {
                    uv[static_cast<size_t>(w_) * r + 2 * x] = r < h_ / 4 ? 90 : 240;
                    uv[static_cast<size_t>(w_) * r + 2 * x + 1] = r < h_ / 4 ? 240 : 110;
                }
            }
            Picture p;
            p.nv12 = f->data();
            p.width = w_, p.height = h_, p.stride = w_;
            p.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch()).count();
            p.hold = f;
            {
                std::lock_guard<std::mutex> hold(lock_);
                if (listener_) listener_(p);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    }
    int w_ = 640, h_ = 480;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex lock_;
    std::function<void(const Picture&)> listener_;
};

bool allowed() { return true; }
bool permitted() { return g_permitted; }
int count() { return 2; }
bool info(int index, Info* out) {
    if (index < 0 || index > 1) return false;
    out->back = index == 0;
    out->name = "Test webcam";
    out->sizes = {{640, 480, 15, 30}, {320, 240, 15, 30}};
    out->autofocus = true;
    out->torch = true;
    return true;
}
std::shared_ptr<Stream> open(int index) { return index >= 0 && index < 2 ? std::make_shared<FakeStream>() : nullptr; }

}  // namespace camera
}  // namespace host
}  // namespace winapk

namespace {

int g_failures = 0;
void check(const char* what, bool ok, const std::string& detail = {}) {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) ++g_failures;
}

template <typename F>
F fn(const char* name) {
    void* p = winapk::host::ndk_camera_resolve(name);
    if (!p) {
        std::printf("  [FAIL] %s is not exported\n", name);
        std::exit(1);
    }
    return reinterpret_cast<F>(p);
}

#define GUEST WINAPK_GUEST_ABI
struct IdList {
    int numCameras;
    const char** cameraIds;
};
struct ConstEntry {
    uint32_t tag;
    uint8_t type;
    uint32_t count;
    const void* data;
};
struct DeviceCallbacks {
    void* context;
    void(GUEST* onDisconnected)(void*, void*);
    void(GUEST* onError)(void*, void*, int);
};
struct SessionCallbacks {
    void* context;
    void(GUEST* onClosed)(void*, void*);
    void(GUEST* onReady)(void*, void*);
    void(GUEST* onActive)(void*, void*);
};
struct CaptureCallbacks {
    void* context;
    void(GUEST* onCaptureStarted)(void*, void*, const void*, int64_t);
    void(GUEST* onCaptureProgressed)(void*, void*, void*, const void*);
    void(GUEST* onCaptureCompleted)(void*, void*, void*, const void*);
    void(GUEST* onCaptureFailed)(void*, void*, void*, void*);
    void(GUEST* onCaptureSequenceCompleted)(void*, void*, int, int64_t);
    void(GUEST* onCaptureSequenceAborted)(void*, void*, int);
    void(GUEST* onCaptureBufferLost)(void*, void*, void*, void*, int64_t);
};
struct ImageListener {
    void* context;
    void(GUEST* onImageAvailable)(void*, void*);
};

struct Ndk {
    void*(GUEST* manager_create)() = fn<decltype(manager_create)>("ACameraManager_create");
    void(GUEST* manager_delete)(void*) = fn<decltype(manager_delete)>("ACameraManager_delete");
    int(GUEST* id_list)(void*, IdList**) = fn<decltype(id_list)>("ACameraManager_getCameraIdList");
    void(GUEST* delete_id_list)(IdList*) = fn<decltype(delete_id_list)>("ACameraManager_deleteCameraIdList");
    int(GUEST* characteristics)(void*, const char*, void**) =
        fn<decltype(characteristics)>("ACameraManager_getCameraCharacteristics");
    int(GUEST* open)(void*, const char*, DeviceCallbacks*, void**) = fn<decltype(open)>("ACameraManager_openCamera");
    int(GUEST* entry)(const void*, uint32_t, ConstEntry*) = fn<decltype(entry)>("ACameraMetadata_getConstEntry");
    void(GUEST* metadata_free)(void*) = fn<decltype(metadata_free)>("ACameraMetadata_free");
    int(GUEST* device_close)(void*) = fn<decltype(device_close)>("ACameraDevice_close");
    int(GUEST* create_request)(void*, int, void**) = fn<decltype(create_request)>("ACameraDevice_createCaptureRequest");
    int(GUEST* request_entry)(const void*, uint32_t, ConstEntry*) =
        fn<decltype(request_entry)>("ACaptureRequest_getConstEntry");
    int(GUEST* set_u8)(void*, uint32_t, uint32_t, const uint8_t*) = fn<decltype(set_u8)>("ACaptureRequest_setEntry_u8");
    void(GUEST* request_free)(void*) = fn<decltype(request_free)>("ACaptureRequest_free");
    void(GUEST* target_free)(void*) = fn<decltype(target_free)>("ACameraOutputTarget_free");
    void(GUEST* output_free)(void*) = fn<decltype(output_free)>("ACaptureSessionOutput_free");
    void(GUEST* container_free)(void*) = fn<decltype(container_free)>("ACaptureSessionOutputContainer_free");
    int(GUEST* target_create)(void*, void**) = fn<decltype(target_create)>("ACameraOutputTarget_create");
    int(GUEST* add_target)(void*, const void*) = fn<decltype(add_target)>("ACaptureRequest_addTarget");
    int(GUEST* container_create)(void**) = fn<decltype(container_create)>("ACaptureSessionOutputContainer_create");
    int(GUEST* output_create)(void*, void**) = fn<decltype(output_create)>("ACaptureSessionOutput_create");
    int(GUEST* container_add)(void*, const void*) = fn<decltype(container_add)>("ACaptureSessionOutputContainer_add");
    int(GUEST* create_session)(void*, const void*, const SessionCallbacks*, void**) =
        fn<decltype(create_session)>("ACameraDevice_createCaptureSession");
    int(GUEST* repeating)(void*, CaptureCallbacks*, int, void**, int*) =
        fn<decltype(repeating)>("ACameraCaptureSession_setRepeatingRequest");
    int(GUEST* capture)(void*, CaptureCallbacks*, int, void**, int*) =
        fn<decltype(capture)>("ACameraCaptureSession_capture");
    int(GUEST* stop_repeating)(void*) = fn<decltype(stop_repeating)>("ACameraCaptureSession_stopRepeating");
    void(GUEST* session_close)(void*) = fn<decltype(session_close)>("ACameraCaptureSession_close");

    int(GUEST* reader_new)(int32_t, int32_t, int32_t, int32_t, void**) = fn<decltype(reader_new)>("AImageReader_new");
    void(GUEST* reader_delete)(void*) = fn<decltype(reader_delete)>("AImageReader_delete");
    int(GUEST* reader_window)(void*, void**) = fn<decltype(reader_window)>("AImageReader_getWindow");
    int(GUEST* reader_listener)(void*, ImageListener*) = fn<decltype(reader_listener)>("AImageReader_setImageListener");
    int(GUEST* acquire_next)(void*, void**) = fn<decltype(acquire_next)>("AImageReader_acquireNextImage");
    int(GUEST* acquire_latest)(void*, void**) = fn<decltype(acquire_latest)>("AImageReader_acquireLatestImage");
    void(GUEST* image_delete)(void*) = fn<decltype(image_delete)>("AImage_delete");
    int(GUEST* image_planes)(const void*, int32_t*) = fn<decltype(image_planes)>("AImage_getNumberOfPlanes");
    int(GUEST* image_width)(const void*, int32_t*) = fn<decltype(image_width)>("AImage_getWidth");
    int(GUEST* image_timestamp)(const void*, int64_t*) = fn<decltype(image_timestamp)>("AImage_getTimestamp");
    int(GUEST* pixel_stride)(const void*, int, int32_t*) = fn<decltype(pixel_stride)>("AImage_getPlanePixelStride");
    int(GUEST* row_stride)(const void*, int, int32_t*) = fn<decltype(row_stride)>("AImage_getPlaneRowStride");
    int(GUEST* plane_data)(const void*, int, uint8_t**, int*) = fn<decltype(plane_data)>("AImage_getPlaneData");
};
Ndk* g_ndk = nullptr;

constexpr uint32_t kLensFacing = 0x80005, kHardwareLevel = 0x150000, kStreamConfigurations = 0xd000a,
                   kFpsRanges = 0x10014, kAfAvailableModes = 0x10017, kAfMode = 0x10007, kFpsRange = 0x10005,
                   kSensorTimestamp = 0xe0010, kFlashMode = 0x40002, kRequestKeys = 0xc000d, kResultKeys = 0xc000e;

// What the callbacks saw.
struct Seen {
    std::mutex lock;
    std::condition_variable cv;
    int ready = 0, active = 0, closed = 0;
    std::set<int64_t> result_timestamps;
    std::vector<std::pair<int, int64_t>> sequences_completed;
    int yuv_images = 0, yuv_good = 0, yuv_timestamped = 0;
    std::string yuv_detail;
    std::vector<int64_t> yuv_timestamps;
    std::vector<uint8_t> jpeg;
    bool jpeg_strides_refused = false;
};
Seen g_seen;
void* g_yuv_reader = nullptr;
void* g_jpeg_reader = nullptr;

template <typename Pred>
bool wait_for(Pred pred, int seconds = 10) {
    std::unique_lock<std::mutex> hold(g_seen.lock);
    return g_seen.cv.wait_for(hold, std::chrono::seconds(seconds), pred);
}

GUEST void on_ready(void*, void*) {
    std::lock_guard<std::mutex> hold(g_seen.lock);
    ++g_seen.ready;
    g_seen.cv.notify_all();
}
GUEST void on_active(void*, void*) {
    std::lock_guard<std::mutex> hold(g_seen.lock);
    ++g_seen.active;
    g_seen.cv.notify_all();
}
GUEST void on_closed(void*, void*) {
    std::lock_guard<std::mutex> hold(g_seen.lock);
    ++g_seen.closed;
    g_seen.cv.notify_all();
}
GUEST void on_completed(void*, void*, void*, const void* result) {
    ConstEntry e{};
    if (g_ndk->entry(result, kSensorTimestamp, &e) == 0 && e.count == 1) {
        std::lock_guard<std::mutex> hold(g_seen.lock);
        g_seen.result_timestamps.insert(*static_cast<const int64_t*>(e.data));
    }
}
GUEST void on_sequence_completed(void*, void*, int sequence, int64_t frame) {
    std::lock_guard<std::mutex> hold(g_seen.lock);
    g_seen.sequences_completed.push_back({sequence, frame});
    g_seen.cv.notify_all();
}

GUEST void on_yuv(void*, void* reader) {
    void* img = nullptr;
    if (g_ndk->acquire_next(reader, &img) != 0) return;
    int32_t planes = 0, w = 0, ps[3] = {}, rs[3] = {};
    int64_t ts = 0;
    uint8_t* data[3] = {};
    int len[3] = {};
    g_ndk->image_planes(img, &planes);
    g_ndk->image_width(img, &w);
    g_ndk->image_timestamp(img, &ts);
    for (int i = 0; i < 3 && i < planes; ++i) {
        g_ndk->pixel_stride(img, i, &ps[i]);
        g_ndk->row_stride(img, i, &rs[i]);
        g_ndk->plane_data(img, i, &data[i], &len[i]);
    }
    // 320x240 (the camera runs at 640x480: scaled), Y 81 over 41, Cb 90 over 240, Cr 240 over 110.
    bool good = planes == 3 && w == 320 && ps[0] == 1 && ps[1] == 2 && ps[2] == 2 && rs[0] == 320 &&
                data[2] == data[1] + 1 && len[0] == 320 * 240;
    char detail[200] = "";
    if (good) {
        const int y_top = data[0][320 * 10 + 10], y_bottom = data[0][320 * 230 + 10];
        const int cb_top = data[1][320 * 5 + 10], cr_top = data[2][320 * 5 + 10];
        const int cb_bottom = data[1][320 * 115 + 10], cr_bottom = data[2][320 * 115 + 10];
        std::snprintf(detail, sizeof detail, "Y %d/%d Cb %d/%d Cr %d/%d", y_top, y_bottom, cb_top, cb_bottom, cr_top,
                      cr_bottom);
        good = y_top == 81 && y_bottom == 41 && cb_top == 90 && cr_top == 240 && cb_bottom == 240 && cr_bottom == 110;
    } else {
        std::snprintf(detail, sizeof detail, "planes %d w %d strides %d/%d/%d row %d", planes, w, ps[0], ps[1], ps[2],
                      rs[0]);
    }
    g_ndk->image_delete(img);
    std::lock_guard<std::mutex> hold(g_seen.lock);
    ++g_seen.yuv_images;
    g_seen.yuv_good += good;
    g_seen.yuv_timestamps.push_back(ts);
    if (g_seen.yuv_detail.empty() || !good) g_seen.yuv_detail = detail;
    g_seen.cv.notify_all();
}
GUEST void on_jpeg(void*, void* reader) {
    void* img = nullptr;
    if (g_ndk->acquire_latest(reader, &img) != 0) return;
    uint8_t* data = nullptr;
    int len = 0;
    int32_t ps = 0;
    g_ndk->plane_data(img, 0, &data, &len);
    const bool refused = g_ndk->pixel_stride(img, 0, &ps) != 0;
    std::lock_guard<std::mutex> hold(g_seen.lock);
    g_seen.jpeg.assign(data, data + len);
    g_seen.jpeg_strides_refused = refused;
    g_ndk->image_delete(img);
    g_seen.cv.notify_all();
}

bool contains(const ConstEntry& e, std::vector<int32_t> want, int stride) {
    const auto* v = static_cast<const int32_t*>(e.data);
    for (uint32_t i = 0; i + stride <= e.count; i += stride) {
        if (std::equal(want.begin(), want.end(), v + i)) return true;
    }
    return false;
}

}  // namespace

int main() {
    Ndk n;
    g_ndk = &n;
    void* manager = n.manager_create();

    std::printf("manager\n");
    IdList* ids = nullptr;
    check("two cameras, \"0\" and \"1\"", n.id_list(manager, &ids) == 0 && ids->numCameras == 2 &&
                                              std::string(ids->cameraIds[0]) == "0" &&
                                              std::string(ids->cameraIds[1]) == "1");
    n.delete_id_list(ids);
    void* chars = nullptr;
    ConstEntry e{};
    check("characteristics of \"0\"", n.characteristics(manager, "0", &chars) == 0 && chars);
    check("\"0\" faces back", n.entry(chars, kLensFacing, &e) == 0 && *static_cast<const uint8_t*>(e.data) == 1);
    check("LEGACY", n.entry(chars, kHardwareLevel, &e) == 0 && *static_cast<const uint8_t*>(e.data) == 2);
    check("stream configurations: YUV 640x480, JPEG 320x240 (AIMAGE_FORMAT_JPEG), PRIVATE",
          n.entry(chars, kStreamConfigurations, &e) == 0 && contains(e, {0x23, 640, 480, 0}, 4) &&
              contains(e, {0x100, 320, 240, 0}, 4) && contains(e, {0x22, 640, 480, 0}, 4));
    check("the frame-rate range 15-30", n.entry(chars, kFpsRanges, &e) == 0 && contains(e, {15, 30}, 2));
    check("autofocus modes", n.entry(chars, kAfAvailableModes, &e) == 0 && e.count == 4);
    check("the request and result keys", n.entry(chars, kRequestKeys, &e) == 0 && contains(e, {int32_t(kFlashMode)}, 1) &&
                                             n.entry(chars, kResultKeys, &e) == 0 &&
                                             contains(e, {int32_t(kSensorTimestamp)}, 1));
    n.metadata_free(chars);
    check("\"1\" faces front", n.characteristics(manager, "1", &chars) == 0 && n.entry(chars, kLensFacing, &e) == 0 &&
                                   *static_cast<const uint8_t*>(e.data) == 0);
    n.metadata_free(chars);
    check("no camera \"2\"", n.characteristics(manager, "2", &chars) != 0);

    DeviceCallbacks dcb{nullptr, nullptr, nullptr};
    void* device = nullptr;
    g_permitted = false;
    check("without CAMERA: PERMISSION_DENIED", n.open(manager, "0", &dcb, &device) == -10013);
    g_permitted = true;
    check("open \"0\"", n.open(manager, "0", &dcb, &device) == 0 && device);

    std::printf("requests and session\n");
    void* preview = nullptr;
    check("a preview request: continuous-picture autofocus, a frame-rate range",
          n.create_request(device, 1, &preview) == 0 && n.request_entry(preview, kAfMode, &e) == 0 &&
              *static_cast<const uint8_t*>(e.data) == 4 && n.request_entry(preview, kFpsRange, &e) == 0 &&
              e.count == 2);
    n.reader_new(320, 240, 0x23, 4, &g_yuv_reader);
    n.reader_new(640, 480, 0x100, 2, &g_jpeg_reader);
    ImageListener yuv_listener{nullptr, on_yuv}, jpeg_listener{nullptr, on_jpeg};
    n.reader_listener(g_yuv_reader, &yuv_listener);
    n.reader_listener(g_jpeg_reader, &jpeg_listener);
    void *yuv_window = nullptr, *jpeg_window = nullptr;
    n.reader_window(g_yuv_reader, &yuv_window);
    n.reader_window(g_jpeg_reader, &jpeg_window);
    void* display = const_cast<void*>(winapk::host::native_window_handle());
    void *container = nullptr, *out1 = nullptr, *out2 = nullptr, *out3 = nullptr;
    n.container_create(&container);
    n.output_create(yuv_window, &out1);
    n.output_create(jpeg_window, &out2);
    n.output_create(display, &out3);
    n.container_add(container, out1);
    n.container_add(container, out2);
    n.container_add(container, out3);
    SessionCallbacks scb{nullptr, on_closed, on_ready, on_active};
    void* session = nullptr;
    check("a session on two readers and the window", n.create_session(device, container, &scb, &session) == 0);
    check("onReady", wait_for([] { return g_seen.ready >= 1; }));

    void *t_yuv = nullptr, *t_display = nullptr, *t_jpeg = nullptr;
    n.target_create(yuv_window, &t_yuv);
    n.target_create(display, &t_display);
    n.target_create(jpeg_window, &t_jpeg);
    n.add_target(preview, t_yuv);
    n.add_target(preview, t_display);
    CaptureCallbacks ccb{};
    ccb.onCaptureCompleted = on_completed;
    ccb.onCaptureSequenceCompleted = on_sequence_completed;
    int repeating_seq = -1;
    check("setRepeatingRequest", n.repeating(session, &ccb, 1, &preview, &repeating_seq) == 0);
    check("onActive", wait_for([] { return g_seen.active >= 1; }));
    const bool five = wait_for([] { return g_seen.yuv_images >= 5; });
    check("five YUV images", five, std::to_string(g_seen.yuv_images));
    {
        std::lock_guard<std::mutex> hold(g_seen.lock);
        check("semiplanar YUV_420_888 at the reader's size, the right colours", g_seen.yuv_good == g_seen.yuv_images,
              g_seen.yuv_detail);
        int matched = 0;
        for (int64_t ts : g_seen.yuv_timestamps) matched += g_seen.result_timestamps.count(ts) ? 1 : 0;
        check("each image's timestamp is a result's SENSOR_TIMESTAMP", matched >= g_seen.yuv_images - 1,
              std::to_string(matched) + " of " + std::to_string(g_seen.yuv_images));
    }
    check("the camera runs at the largest reader's size", g_configured_w == 640 && g_configured_h == 480,
          std::to_string(g_configured_w) + "x" + std::to_string(g_configured_h));
    check("pictures on the window", g_shown >= 3, std::to_string(g_shown));
    check("continuous autofocus on", g_focus_continuous.load());

    std::printf("capture, torch, readers\n");
    void* still = nullptr;
    n.create_request(device, 2, &still);
    n.add_target(still, t_jpeg);
    const uint8_t torch = 2;
    n.set_u8(still, kFlashMode, 1, &torch);
    int still_seq = -1;
    check("capture a JPEG", n.capture(session, &ccb, 1, &still, &still_seq) == 0 && still_seq != repeating_seq);
    check("a whole JPEG", wait_for([] {
              return g_seen.jpeg.size() > 100 && g_seen.jpeg[0] == 0xff && g_seen.jpeg[1] == 0xd8 &&
                     g_seen.jpeg[g_seen.jpeg.size() - 2] == 0xff && g_seen.jpeg.back() == 0xd9;
          }),
          std::to_string(g_seen.jpeg.size()) + " bytes");
    check("JPEG planes have no strides", g_seen.jpeg_strides_refused);
    check("the capture's sequence completes", wait_for([&] {
        for (auto& s : g_seen.sequences_completed) if (s.first == still_seq) return true;
        return false;
    }));
    check("FLASH_MODE_TORCH turned the torch on", g_torch_was_on.load());

    void* empty_reader = nullptr;
    void* img = nullptr;
    n.reader_new(64, 48, 0x23, 1, &empty_reader);
    check("no image yet: NO_BUFFER_AVAILABLE", n.acquire_next(empty_reader, &img) == -30001);
    n.reader_delete(empty_reader);

    std::printf("stop and close\n");
    const int ready_before = g_seen.ready;
    check("stopRepeating", n.stop_repeating(session) == 0);
    check("the repeating sequence completes", wait_for([&] {
        for (auto& s : g_seen.sequences_completed) if (s.first == repeating_seq) return true;
        return false;
    }));
    check("onReady again", wait_for([&] { return g_seen.ready > ready_before; }));
    n.session_close(session);
    check("onClosed", wait_for([] { return g_seen.closed >= 1; }));
    check("close the device", n.device_close(device) == 0);
    check("the camera stopped", g_stopped >= 1 && g_started >= 1);
    n.request_free(preview);
    n.request_free(still);
    for (void* t : {t_yuv, t_display, t_jpeg}) n.target_free(t);
    for (void* o : {out1, out2, out3}) n.output_free(o);
    n.container_free(container);
    n.reader_delete(g_yuv_reader);
    n.reader_delete(g_jpeg_reader);
    n.manager_delete(manager);

    std::printf(g_failures ? "%d FAILED\n" : "ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
