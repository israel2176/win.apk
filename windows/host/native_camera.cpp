// native_camera.cpp -- com.macapk.loader.CameraBridge on Windows: the PC's
// webcams through Media Foundation (upstream: camera_capture.mm on
// AVFoundation). camera2, android.hardware.Camera, ImageReader and
// SurfaceTexture sit on top of these natives in runtime/java.
//
// Devices are the video capture sources MFEnumDeviceSources lists, each offered
// to the app as a back and a front camera (see kRolesPerDevice). A source
// reader with advanced
// video processing turns whatever the device sends (YUY2, MJPG, NV12...) into
// NV12, which a capture thread copies into a frame of our own: Y rows, then
// interleaved CbCr rows, both `stride` bytes wide. That is the semiplanar
// YUV_420_888 layout CameraBridge describes -- plane 1 (U) and plane 2 (V) are
// views of the same chroma plane one byte apart, pixel stride 2. Owning the
// copy (rather than handing out the locked IMFMediaBuffer) lets a held Image,
// the newest frame for SurfaceTexture and the capture thread each keep a frame
// alive without pinning the reader's buffers.
//
// Frames are full range (JFIF), which is what Android's YUV consumers
// (YuvImage, the common shaders) assume. Media Foundation hands out
// video-range NV12 unless asked otherwise; when the converter will not do the
// expansion itself, the copy does it.
//
// Windows does not prompt a desktop program for the camera: the privacy
// switches (Settings > Privacy > Camera) either let it open the device or not,
// so nativeAuthStatus reads them and nativeRequestAuth has nothing to ask.

#include "host_natives.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <objidl.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <strmif.h>

#include "camera_host.h"
#include "win32_window.h"

namespace winapk::host {

namespace {

// CameraBridge constants (AOSP's LENS_FACING_FRONT; AVAuthorizationStatus).
constexpr jint kFacingFront = 0, kFacingBack = 1;
constexpr jint kAuthDenied = 2, kAuthAuthorized = 3;
constexpr DWORD kStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

template <typename T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// Media Foundation objects are free-threaded, but the calls that create them
// need COM on the calling thread, and a Java thread may already be a
// single-threaded apartment (or not initialized at all). Running those calls
// on a short-lived MTA thread of our own sidesteps both.
template <typename F>
void on_mta(F&& fn) {
    std::thread t([&] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        fn();
        if (SUCCEEDED(co)) CoUninitialize();
    });
    t.join();
}

void mf_startup() {
    static std::once_flag once;
    std::call_once(once, [] { MFStartup(MF_VERSION, MFSTARTUP_LITE); });
}

std::string utf8(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// The two camera switches a desktop program is subject to: "Camera access"
// (the device-wide one, per machine or per user) and "Let desktop apps access
// your camera" (NonPackaged). Either one set to Deny makes the device fail to
// open, so it is reported as denied up front instead.
bool camera_allowed() {
    const wchar_t* base =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam";
    const wchar_t* desktop =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam\\NonPackaged";
    const std::pair<HKEY, const wchar_t*> keys[] = {
        {HKEY_LOCAL_MACHINE, base}, {HKEY_CURRENT_USER, base}, {HKEY_CURRENT_USER, desktop}};
    for (const auto& [root, key] : keys) {
        wchar_t value[32] = {};
        DWORD size = sizeof(value);
        if (RegGetValueW(root, key, L"Value", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS &&
            lstrcmpiW(value, L"Deny") == 0) {
            return false;
        }
    }
    return true;
}

// ---- devices ----------------------------------------------------------------

struct Format {
    int width = 0, height = 0;
    double min_fps = 0, max_fps = 0;
};

struct Device {
    std::wstring link;  // symbolic link: stable, unique, what MFCreateDeviceSource opens
    std::string id, name;
    bool probed = false;
    std::vector<Format> formats;  // distinct sizes, largest first
    jint controls = 0;            // kCtl* bits, probed with the formats
};

std::mutex g_devices_lock;
std::vector<Device> g_devices;
std::chrono::steady_clock::time_point g_enumerated_at;

// The device subtypes the source reader's converters are known to turn into
// NV12. Anything else (H.264 webcams' second stream, depth, IR) is not offered.
bool convertible(const GUID& subtype) {
    for (const GUID& g : {MFVideoFormat_NV12, MFVideoFormat_YUY2, MFVideoFormat_MJPG, MFVideoFormat_I420,
                          MFVideoFormat_IYUV, MFVideoFormat_YV12, MFVideoFormat_UYVY, MFVideoFormat_RGB24,
                          MFVideoFormat_RGB32}) {
        if (subtype == g) return true;
    }
    return false;
}

// Lower is better at equal size and rate: the formats needing no decode.
int subtype_rank(const GUID& subtype) {
    if (subtype == MFVideoFormat_NV12) return 0;
    if (subtype == MFVideoFormat_YUY2) return 1;
    if (subtype == MFVideoFormat_MJPG) return 3;
    return 2;
}

double type_fps(IMFMediaType* t, const GUID& key) {
    UINT32 num = 0, den = 0;
    if (FAILED(MFGetAttributeRatio(t, key, &num, &den)) || den == 0) return 0;
    return static_cast<double>(num) / den;
}

// Opens the device as a media source and a source reader over it. The reader
// converts to NV12 (MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING inserts
// the MJPG decoder and the color converter as needed).
bool open_reader(const std::wstring& link, Com<IMFMediaSource>& source, Com<IMFSourceReader>& reader) {
    mf_startup();
    Com<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 2)) ||
        FAILED(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)) ||
        FAILED(attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.c_str())) ||
        FAILED(MFCreateDeviceSource(attrs.p, &source))) {
        return false;
    }
    Com<IMFAttributes> rattrs;
    if (FAILED(MFCreateAttributes(&rattrs, 2)) ||
        FAILED(rattrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE)) ||
        FAILED(rattrs->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE)) ||
        FAILED(MFCreateSourceReaderFromMediaSource(source.p, rattrs.p, &reader))) {
        source->Shutdown();
        source.reset();
        return false;
    }
    return true;
}

// ---- focus and torch --------------------------------------------------------
//
// What a webcam lets a program steer, asked of the driver, never assumed:
//
//  * IAMCameraControl's CameraControl_Focus, the UVC focus control
//    (CT_FOCUS_ABSOLUTE / CT_FOCUS_AUTO): GetRange's flags say whether the
//    camera focuses by itself (CameraControl_Flags_Auto), and Get reads the lens
//    position while it does. Most autofocus webcams have this and nothing else.
//  * The extended camera controls (KSPROPERTYSETID_ExtendedCameraControl, the
//    Windows 8.1 camera driver model): FOCUSMODE with a one-shot (AUTO | LOCK)
//    and a continuous mode, FOCUSSTATE (searching / focused / failed), and
//    TORCHMODE. Tablets' and 2-in-1s' built-in cameras have these.
//
// A camera is reported as autofocusing (camera1's "auto", camera2's AF modes,
// android.hardware.camera.autofocus) only when a focus can be both started and
// observed: the extended one-shot with its focus state, or the UVC auto flag
// with a readable lens position. A torch only with TORCHMODE's ON capability.
// Everything else is a fixed-focus camera without a flash, which is what the
// app is told.
//
// No distance comes out of either: UVC's focus unit is nominally millimetres
// but webcams report their own scale (0..255 is common), so camera2's
// LENS_INFO_MINIMUM_FOCUS_DISTANCE would be invented and is left out for an
// autofocus camera (it is optional in AOSP), and 0 (fixed focus) otherwise.

// CameraBridge.CONTROL_* bits.
constexpr jint kCtlAutofocus = 1, kCtlTorch = 2;
// CameraBridge.FOCUS_HOLD / FOCUS_CONTINUOUS.
constexpr int kFocusHold = 0, kFocusContinuous = 1;

// The extended controls' wire format, from the Windows SDK's ksmedia.h (not in
// MinGW's): the property set, the ids used here, the header every extended
// property starts with, and the flags.
const GUID kExtendedCameraControl = {0x1CB79112, 0xC0D2, 0x4213, {0x9C, 0xA6, 0xCD, 0x4F, 0xDB, 0x92, 0x79, 0x72}};
constexpr ULONG kExtTorchMode = 8, kExtFocusMode = 13, kExtFocusState = 20;
constexpr ULONG kExtFilterScope = 0xFFFFFFFF;
constexpr ULONGLONG kVideoProcAuto = 0x1, kVideoProcLock = 0x4;
constexpr ULONGLONG kExtFocusContinuous = 0x100, kFocusRangeFull = 0x40000;
constexpr ULONGLONG kTorchOff = 0x0, kTorchOn = 0x1;
enum : ULONGLONG {
    kFocusStateUninitialized,
    kFocusStateLost,
    kFocusStateSearching,
    kFocusStateFocused,
    kFocusStateFailed
};
constexpr ULONG kKsGet = 0x1, kKsSet = 0x2;  // KSPROPERTY_TYPE_GET / _SET

struct KsIdentifier {  // KSPROPERTY
    GUID set;
    ULONG id;
    ULONG flags;
};
struct ExtHeader {  // KSCAMERA_EXTENDEDPROP_HEADER
    ULONG version, pin_id, size, result;
    ULONGLONG flags, capability;
};
struct ExtVideoProc {  // KSCAMERA_EXTENDEDPROP_VIDEOPROCSETTING
    ULONG mode;
    LONG min, max, step;
    ULONGLONG value;
    ULONGLONG reserved;
};
static_assert(sizeof(ExtHeader) == 32 && sizeof(ExtVideoProc) == 32, "ksmedia.h layouts");

// IKsControl as ks.h declares it (ks.h itself pulls in the kernel-streaming
// headers' NTSTATUS world; the vtable is all that matters here).
struct IKsControlMin : public IUnknown {
    virtual LONG STDMETHODCALLTYPE KsProperty(KsIdentifier*, ULONG, void*, ULONG, ULONG*) = 0;
    virtual LONG STDMETHODCALLTYPE KsMethod(KsIdentifier*, ULONG, void*, ULONG, ULONG*) = 0;
    virtual LONG STDMETHODCALLTYPE KsEvent(KsIdentifier*, ULONG, void*, ULONG, ULONG*) = 0;
};
const IID kIID_IKsControl = {0x28F54685, 0x06FD, 0x11D2, {0xB2, 0x7A, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96}};
const IID kIID_IAMCameraControl = {0xC6E13370, 0x30AC, 0x11D0, {0xA1, 0x8C, 0x00, 0xA0, 0xC9, 0x11, 0x89, 0x56}};

// GET of an extended property: the header and `payload_size` bytes after it.
bool ext_get(IKsControlMin* ks, ULONG id, ExtHeader* hdr, void* payload, ULONG payload_size) {
    std::vector<uint8_t> buf(sizeof(ExtHeader) + payload_size);
    KsIdentifier prop{kExtendedCameraControl, id, kKsGet};
    ULONG got = 0;
    if (ks->KsProperty(&prop, sizeof(prop), buf.data(), static_cast<ULONG>(buf.size()), &got) < 0 ||
        got < sizeof(ExtHeader)) {
        return false;
    }
    std::memcpy(hdr, buf.data(), sizeof(ExtHeader));
    if (payload) std::memcpy(payload, buf.data() + sizeof(ExtHeader), payload_size);
    return true;
}

bool ext_set(IKsControlMin* ks, ULONG id, ULONGLONG flags, const void* payload, ULONG payload_size) {
    std::vector<uint8_t> buf(sizeof(ExtHeader) + payload_size);
    ExtHeader hdr{};
    hdr.version = 1;
    hdr.pin_id = kExtFilterScope;
    hdr.size = static_cast<ULONG>(buf.size());
    hdr.flags = flags;
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    if (payload) std::memcpy(buf.data() + sizeof(hdr), payload, payload_size);
    KsIdentifier prop{kExtendedCameraControl, id, kKsSet};
    ULONG got = 0;
    return ks->KsProperty(&prop, sizeof(prop), buf.data(), static_cast<ULONG>(buf.size()), &got) >= 0;
}

// The controls of one open media source. Used on an MTA thread only.
struct Controls {
    Com<IAMCameraControl> uvc;
    Com<IKsControlMin> ks;
    bool uvc_auto = false;    // the camera focuses by itself
    bool uvc_manual = false;  // and can be held where it is
    bool uvc_read = false;    // Get reports the lens position
    bool ext_focus = false;   // extended FOCUSMODE: one-shot and continuous
    bool ext_state = false;   // extended FOCUSSTATE
    bool ext_torch = false;   // extended TORCHMODE with ON

    void open(IMFMediaSource* source) {
        if (SUCCEEDED(source->QueryInterface(kIID_IAMCameraControl, reinterpret_cast<void**>(&uvc.p)))) {
            long mn = 0, mx = 0, step = 0, def = 0, caps = 0, value = 0, flags = 0;
            if (SUCCEEDED(uvc->GetRange(CameraControl_Focus, &mn, &mx, &step, &def, &caps))) {
                uvc_auto = (caps & CameraControl_Flags_Auto) != 0;
                uvc_manual = (caps & CameraControl_Flags_Manual) != 0 && mx > mn;
                uvc_read = SUCCEEDED(uvc->Get(CameraControl_Focus, &value, &flags));
            }
        }
        if (SUCCEEDED(source->QueryInterface(kIID_IKsControl, reinterpret_cast<void**>(&ks.p)))) {
            ExtHeader h{};
            ExtVideoProc vp{};
            ext_focus = ext_get(ks.p, kExtFocusMode, &h, &vp, sizeof(vp)) && (h.capability & kVideoProcAuto) &&
                        (h.capability & kVideoProcLock) && (h.capability & kExtFocusContinuous);
            ULONGLONG v = 0;
            ext_state = ext_get(ks.p, kExtFocusState, &h, &v, sizeof(v));
            ext_torch = ext_get(ks.p, kExtTorchMode, &h, &v, sizeof(v)) && (h.capability & kTorchOn);
        }
    }

    // A focus that can be started and seen to finish.
    bool use_ext() const { return ext_focus && ext_state; }
    bool autofocus() const { return use_ext() || (uvc_auto && uvc_read); }
    jint mask() const { return (autofocus() ? kCtlAutofocus : 0) | (ext_torch ? kCtlTorch : 0); }

    bool lens(long* value) {
        long flags = 0;
        return uvc_read && SUCCEEDED(uvc->Get(CameraControl_Focus, value, &flags));
    }
    // Lets the camera focus by itself, continuously.
    void continuous() {
        if (use_ext()) {
            ExtVideoProc vp{};
            ext_set(ks.p, kExtFocusMode, kExtFocusContinuous | kFocusRangeFull, &vp, sizeof(vp));
        } else if (uvc_auto) {
            long v = 0;
            lens(&v);
            uvc->Set(CameraControl_Focus, v, CameraControl_Flags_Auto);
        }
    }
    // Keeps the lens where it is.
    void hold() {
        if (use_ext()) {
            ExtVideoProc vp{};
            ext_set(ks.p, kExtFocusMode, kVideoProcLock, &vp, sizeof(vp));
        } else if (uvc_manual) {
            long v = 0;
            if (lens(&v)) uvc->Set(CameraControl_Focus, v, CameraControl_Flags_Manual);
        }
    }
    // Starts a one-shot focus: the extended AUTO | LOCK (the driver converges
    // and locks), or UVC's auto focus, which hold() locks once the lens settles.
    void single() {
        if (use_ext()) {
            ExtVideoProc vp{};
            ext_set(ks.p, kExtFocusMode, kVideoProcAuto | kVideoProcLock | kFocusRangeFull, &vp, sizeof(vp));
        } else {
            continuous();
        }
    }
    ULONGLONG focus_state() {
        ExtHeader h{};
        ULONGLONG v = 0;
        return ext_state && ext_get(ks.p, kExtFocusState, &h, &v, sizeof(v)) ? h.flags : kFocusStateUninitialized;
    }
    bool torch(bool on) {
        if (!ext_torch) return false;
        ULONGLONG power = on ? 100 : 0;
        return ext_set(ks.p, kExtTorchMode, on ? kTorchOn : kTorchOff, &power, sizeof(power));
    }
};

// The sizes and frame-rate ranges the device advertises. Opening the device is
// the only way Media Foundation lists them, so it is done once per device, on
// demand, and also serves as open()'s proof that the device can be opened.
std::vector<Format> probe_formats(const std::wstring& link, jint* controls) {
    std::vector<Format> out;
    *controls = 0;
    on_mta([&] {
        Com<IMFMediaSource> source;
        Com<IMFSourceReader> reader;
        if (!open_reader(link, source, reader)) return;
        {
            Controls c;
            c.open(source.p);
            *controls = c.mask();
        }
        for (DWORD i = 0;; ++i) {
            Com<IMFMediaType> t;
            if (FAILED(reader->GetNativeMediaType(kStream, i, &t))) break;
            GUID subtype{};
            UINT32 w = 0, h = 0;
            if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &subtype)) || !convertible(subtype) ||
                FAILED(MFGetAttributeSize(t.p, MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0 || (w | h) & 1) {
                continue;
            }
            const double fps = type_fps(t.p, MF_MT_FRAME_RATE);
            double lo = type_fps(t.p, MF_MT_FRAME_RATE_RANGE_MIN), hi = type_fps(t.p, MF_MT_FRAME_RATE_RANGE_MAX);
            if (lo <= 0) lo = fps;
            if (hi <= 0) hi = fps;
            auto it = std::find_if(out.begin(), out.end(), [&](const Format& f) {
                return f.width == static_cast<int>(w) && f.height == static_cast<int>(h);
            });
            if (it == out.end()) {
                out.push_back({static_cast<int>(w), static_cast<int>(h), lo, hi});
            } else {
                it->min_fps = std::min(it->min_fps, lo);
                it->max_fps = std::max(it->max_fps, hi);
            }
        }
        reader.reset();
        source->Shutdown();
    });
    std::sort(out.begin(), out.end(), [](const Format& a, const Format& b) {
        return static_cast<long long>(a.width) * a.height > static_cast<long long>(b.width) * b.height;
    });
    return out;
}

// Re-lists the devices (at most once a second: camera2 asks for the count on
// nearly every call). Devices still present keep their probed formats.
void refresh_devices(bool force) {
    std::lock_guard<std::mutex> hold(g_devices_lock);
    const auto now = std::chrono::steady_clock::now();
    if (!force && g_enumerated_at.time_since_epoch().count() != 0 && now - g_enumerated_at < std::chrono::seconds(1)) {
        return;
    }
    g_enumerated_at = now;
    std::vector<Device> found;
    on_mta([&] {
        mf_startup();
        Com<IMFAttributes> attrs;
        IMFActivate** list = nullptr;
        UINT32 count = 0;
        if (FAILED(MFCreateAttributes(&attrs, 1)) ||
            FAILED(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                  MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)) ||
            FAILED(MFEnumDeviceSources(attrs.p, &list, &count))) {
            return;
        }
        for (UINT32 i = 0; i < count; ++i) {
            wchar_t* link = nullptr;
            wchar_t* name = nullptr;
            UINT32 len = 0;
            if (SUCCEEDED(list[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link,
                                                      &len))) {
                list[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
                Device d;
                d.link = link;
                d.id = utf8(link);
                d.name = name ? utf8(name) : "Camera";
                found.push_back(std::move(d));
            }
            CoTaskMemFree(link);
            CoTaskMemFree(name);
            list[i]->Release();
        }
        CoTaskMemFree(list);
    });
    for (Device& d : found) {
        for (Device& old : g_devices) {
            if (old.link == d.link) {
                d.probed = old.probed;
                d.formats = std::move(old.formats);
                d.controls = old.controls;
            }
        }
    }
    g_devices = std::move(found);
}

// Every webcam is offered twice: camera 2k faces BACK and 2k+1 faces FRONT,
// both streaming the same device. By design a PC's camera serves both
// roles, so apps that ask for the rear camera (cheque scanners) work.
// (AOSP would report a USB camera as EXTERNAL, which camera1 maps to FRONT
// and an app asking for the back lens would not find.) A phone has both and apps are written for
// that: QR scanners and CameraX's DEFAULT_BACK_CAMERA ask for the back lens
// (Camera.open() returns null without one), selfie and video-call screens for
// the front. A PC's single webcam serves both roles; Windows shares the device
// between the two if an app opens both.
constexpr int kRolesPerDevice = 2;

// The webcam behind camera `index`, or -1. Under g_devices_lock.
int physical_index(int index) {
    return index >= 0 && static_cast<size_t>(index / kRolesPerDevice) < g_devices.size() ? index / kRolesPerDevice
                                                                                            : -1;
}

// A copy of the webcam behind camera `index`, its formats probed. False if
// there is no such camera.
bool device_at(int index, Device* out) {
    std::lock_guard<std::mutex> hold(g_devices_lock);
    const int phys = physical_index(index);
    if (phys < 0) return false;
    Device& d = g_devices[static_cast<size_t>(phys)];
    if (!d.probed) {
        d.formats = probe_formats(d.link, &d.controls);
        // A device that failed to open (in use exclusively, unplugged) is
        // probed again next time rather than remembered as format-less.
        d.probed = !d.formats.empty();
    }
    *out = d;
    return true;
}

bool format_at(int device, int format, Format* out) {
    Device d;
    if (!device_at(device, &d) || format < 0 || static_cast<size_t>(format) >= d.formats.size()) return false;
    *out = d.formats[static_cast<size_t>(format)];
    return true;
}

// ---- sessions -----------------------------------------------------------------

// One NV12 frame: `height` rows of Y, then height/2 rows of CbCr pairs, all
// `stride` bytes apart. Always full range.
struct Frame {
    std::vector<uint8_t> data;
    int width = 0, height = 0, stride = 0;
    int64_t pts = 0;
    int64_t number = 0;  // set when acquired
};

int64_t now_ns() {
    // steady_clock is QueryPerformanceCounter here, the clock System.nanoTime
    // reads, so timestamps are at least comparable with Java's.
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Video range (Y 16..235, C 16..240) to full range, by table.
struct RangeTables {
    uint8_t luma[256], chroma[256];
    RangeTables() {
        for (int v = 0; v < 256; ++v) {
            luma[v] = static_cast<uint8_t>(std::clamp((v - 16) * 255 / 219, 0, 255));
            chroma[v] = static_cast<uint8_t>(std::clamp(128 + (v - 128) * 255 / 224, 0, 255));
        }
    }
};
const RangeTables& range_tables() {
    static const RangeTables t;
    return t;
}

class Session {
public:
    Session(int device, std::wstring link, std::vector<Format> formats)
        : device_(device), link_(std::move(link)), formats_(std::move(formats)) {
        choose(1280, 720, 30);
    }
    ~Session() { stop(); }

    int device() const { return device_; }

    // Picks the advertised size closest in area to the request and clamps the
    // rate into its range -- what the app gets, even when it asked for a size
    // the camera does not have.
    bool configure(int w, int h, int fps) {
        if (formats_.empty()) return false;
        const bool was_running = running();
        if (was_running) stop();
        choose(w, h, fps);
        return was_running ? start() : true;
    }
    int width() {
        std::lock_guard<std::mutex> hold(lock_);
        return width_;
    }
    int height() {
        std::lock_guard<std::mutex> hold(lock_);
        return height_;
    }

    // Starts the capture thread and waits for the device to open (or fail).
    bool start() {
        std::lock_guard<std::mutex> guard(run_lock_);
        if (thread_.joinable()) return true;
        {
            std::lock_guard<std::mutex> hold(state_lock_);
            opened_ = failed_ = done_ = false;
        }
        stopping_ = false;
        thread_ = std::thread([this] { run(); });
        std::unique_lock<std::mutex> wait(state_lock_);
        state_cv_.wait_for(wait, std::chrono::seconds(10), [this] { return opened_ || failed_; });
        const bool ok = opened_;
        wait.unlock();
        if (!ok) stop_locked();
        return ok;
    }

    void stop() {
        std::lock_guard<std::mutex> guard(run_lock_);
        stop_locked();
    }

    bool running() {
        std::lock_guard<std::mutex> guard(run_lock_);
        return thread_.joinable();
    }

    int64_t delivered() const { return delivered_.load(); }

    // Every frame, as it arrives, on the capture thread (camera_host.h's Stream).
    void set_listener(std::function<void(const std::shared_ptr<Frame>&)> listener) {
        std::lock_guard<std::mutex> hold(listener_lock_);
        listener_ = std::move(listener);
    }

    // ---- frame handoff (ImageReader.acquireLatestImage / Image.close) ----
    bool acquire() {
        std::lock_guard<std::mutex> hold(lock_);
        if (held_ || !pending_) return false;
        held_ = std::move(pending_);
        held_->number = ++acquired_;
        return true;
    }
    void release() {
        std::lock_guard<std::mutex> hold(lock_);
        held_.reset();
    }
    std::shared_ptr<Frame> held() {
        std::lock_guard<std::mutex> hold(lock_);
        return held_;
    }
    std::shared_ptr<Frame> latest() {
        std::lock_guard<std::mutex> hold(lock_);
        return latest_;
    }

    // ---- focus and torch ----
    //
    // The device's controls belong to the capture thread's media source, so the
    // calls below only post a request; the capture thread carries it out between
    // frames (control_tick) and answers. Without a running capture there is no
    // source to steer: AOSP's autoFocus is only valid between startPreview and
    // stopPreview, and camera2 focuses as part of a running request.
    jint controls() const { return controls_.load(); }

    // kFocusHold: the lens stays where it is. kFocusContinuous: the camera
    // keeps focusing by itself. Remembered, and applied when capture starts.
    void set_focus_mode(int mode) {
        std::lock_guard<std::mutex> hold(ctl_lock_);
        focus_mode_ = mode;
        ++focus_mode_gen_;
    }

    // A one-shot focus: 1 focused (and the lens then held), 0 not focused,
    // -1 not possible (no autofocus, not capturing) or cancelled.
    int autofocus(int timeout_ms) {
        std::unique_lock<std::mutex> wait(ctl_lock_);
        if (!capturing_ || !(controls_.load() & kCtlAutofocus)) return -1;
        const uint64_t gen = ++af_req_gen_;
        af_timeout_ms_ = std::max(timeout_ms, 100);
        const bool answered = ctl_cv_.wait_for(wait, std::chrono::milliseconds(af_timeout_ms_ + 2000),
                                               [&] { return af_done_gen_ >= gen || !capturing_; });
        return answered && af_done_gen_ >= gen ? af_result_ : -1;
    }

    void cancel_autofocus() {
        std::lock_guard<std::mutex> hold(ctl_lock_);
        ++af_cancel_gen_;
    }

    // 0 unknown, 1 searching, 2 focused, 3 failed: what the camera reports
    // while it focuses by itself (the extended focus state), or, for a UVC
    // webcam, whether its lens position is moving (searching) or has settled.
    int focus_state() const { return focus_state_.load(); }

    bool set_torch(bool on) {
        std::unique_lock<std::mutex> wait(ctl_lock_);
        if (!capturing_ || !(controls_.load() & kCtlTorch)) return false;
        torch_want_ = on;
        const uint64_t gen = ++torch_gen_;
        ctl_cv_.wait_for(wait, std::chrono::seconds(2), [&] { return torch_done_gen_ >= gen || !capturing_; });
        return torch_done_gen_ >= gen && torch_ok_;
    }

private:
    // The capture thread's side of the requests above, once per frame.
    void control_tick(Controls& c, std::chrono::steady_clock::time_point now) {
        using std::chrono::milliseconds;
        uint64_t mode_gen, req_gen, cancel_gen, torch_gen;
        int mode, timeout_ms;
        bool torch_want;
        {
            std::lock_guard<std::mutex> hold(ctl_lock_);
            mode_gen = focus_mode_gen_;
            mode = focus_mode_;
            req_gen = af_req_gen_;
            cancel_gen = af_cancel_gen_;
            timeout_ms = af_timeout_ms_;
            torch_gen = torch_gen_;
            torch_want = torch_want_;
        }
        if (torch_gen != torch_applied_gen_) {
            torch_applied_gen_ = torch_gen;
            const bool ok = c.torch(torch_want);
            std::lock_guard<std::mutex> hold(ctl_lock_);
            torch_ok_ = ok;
            torch_done_gen_ = torch_gen;
            ctl_cv_.notify_all();
        }
        if (cancel_gen != af_cancel_seen_) {
            af_cancel_seen_ = cancel_gen;
            if (af_running_) {
                af_running_ = false;
                finish_autofocus(-1);
            }
            mode_applied_gen_ = 0;  // back to the requested mode
        }
        if (mode_gen != mode_applied_gen_ && !af_running_) {
            mode_applied_gen_ = mode_gen;
            if (mode == kFocusContinuous) c.continuous(); else c.hold();
            moved_at_ = now;
        }
        if (req_gen != af_started_gen_) {
            af_started_gen_ = req_gen;
            if (!c.autofocus()) {
                finish_autofocus(-1);
            } else {
                af_running_ = true;
                af_moved_ = af_seen_search_ = false;
                af_start_ = now;
                c.single();
            }
        }
        long lens = 0;
        const bool have_lens = c.lens(&lens);
        if (have_lens && lens != last_lens_) {
            last_lens_ = lens;
            moved_at_ = now;
            if (af_running_) af_moved_ = true;
        }
        const ULONGLONG st = c.focus_state();
        int passive = 0;
        if (c.use_ext()) {
            passive = st == kFocusStateSearching ? 1
                      : st == kFocusStateFocused ? 2
                      : st == kFocusStateFailed || st == kFocusStateLost ? 3 : 0;
        } else if (have_lens) {
            passive = now - moved_at_ < milliseconds(300) ? 1 : 2;
        }
        focus_state_ = passive;
        if (!af_running_) return;
        const auto elapsed = now - af_start_;
        int result = -2;  // still focusing
        if (c.use_ext()) {
            // The state can still read FOCUSED from before the request for a
            // moment; a search seen, or 300 ms, makes it this focus's answer.
            if (st == kFocusStateSearching) af_seen_search_ = true;
            const bool current = af_seen_search_ || elapsed >= milliseconds(300);
            if (current && st == kFocusStateFocused) result = 1;
            else if (current && (st == kFocusStateFailed || st == kFocusStateLost)) result = 0;
        } else if (af_moved_ ? now - moved_at_ >= milliseconds(300) : elapsed >= milliseconds(600)) {
            // The lens moved and has stopped, or never had to move.
            result = 1;
        }
        if (result == -2 && elapsed >= milliseconds(timeout_ms)) result = 0;
        if (result != -2) {
            af_running_ = false;
            c.hold();  // AOSP: the lens stays put after autoFocus until cancelAutoFocus
            finish_autofocus(result);
        }
    }

    void finish_autofocus(int result) {
        std::lock_guard<std::mutex> hold(ctl_lock_);
        af_result_ = result;
        af_done_gen_ = af_started_gen_;
        ctl_cv_.notify_all();
    }

    void set_capturing(bool on) {
        std::lock_guard<std::mutex> hold(ctl_lock_);
        capturing_ = on;
        if (on) {
            // A new capture: the mode is applied afresh; nothing is pending.
            mode_applied_gen_ = 0;
            af_started_gen_ = af_req_gen_;
            af_cancel_seen_ = af_cancel_gen_;
            torch_applied_gen_ = torch_gen_;
            af_running_ = false;
            moved_at_ = std::chrono::steady_clock::now();
        } else {
            // A request the thread will never see is answered here.
            af_done_gen_ = af_req_gen_;
            af_result_ = -1;
            torch_done_gen_ = torch_gen_;
            torch_ok_ = false;
        }
        ctl_cv_.notify_all();
    }

    void choose(int w, int h, int fps) {
        const long long want = static_cast<long long>(std::max(w, 1)) * std::max(h, 1);
        const Format* best = nullptr;
        for (const Format& f : formats_) {
            const long long delta = std::llabs(static_cast<long long>(f.width) * f.height - want);
            if (!best || delta < std::llabs(static_cast<long long>(best->width) * best->height - want)) best = &f;
        }
        std::lock_guard<std::mutex> hold(lock_);
        if (!best) return;
        width_ = best->width;
        height_ = best->height;
        fps_ = fps > 0 ? std::clamp(static_cast<double>(fps), best->min_fps, std::max(best->min_fps, best->max_fps))
                       : best->max_fps;
    }

    void stop_locked() {
        if (!thread_.joinable()) return;
        stopping_ = true;
        // ReadSample blocks until the next frame, normally a frame time. A
        // camera that stops delivering would block it for good, so after a
        // grace period the source is shut down, which fails the read.
        {
            std::unique_lock<std::mutex> wait(state_lock_);
            if (!state_cv_.wait_for(wait, std::chrono::seconds(2), [this] { return done_; }) && source_) {
                source_->Shutdown();
            }
        }
        thread_.join();
        // A stopped camera must not keep serving its last frame as if live.
        std::lock_guard<std::mutex> hold(lock_);
        pending_.reset();
        latest_.reset();
    }

    void signal(bool opened) {
        std::lock_guard<std::mutex> hold(state_lock_);
        (opened ? opened_ : failed_) = true;
        state_cv_.notify_all();
    }

    // The device type matching the chosen size whose rate is the lowest at or
    // above the wanted one (else the highest), preferring formats that need no
    // decode.
    bool select_native_type(IMFSourceReader* reader, int w, int h, double fps) {
        Com<IMFMediaType> best;
        double best_fps = 0;
        int best_rank = 0;
        for (DWORD i = 0;; ++i) {
            Com<IMFMediaType> t;
            if (FAILED(reader->GetNativeMediaType(kStream, i, &t))) break;
            GUID subtype{};
            UINT32 tw = 0, th = 0;
            if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &subtype)) || !convertible(subtype) ||
                FAILED(MFGetAttributeSize(t.p, MF_MT_FRAME_SIZE, &tw, &th)) || static_cast<int>(tw) != w ||
                static_cast<int>(th) != h) {
                continue;
            }
            const double f = type_fps(t.p, MF_MT_FRAME_RATE);
            const int rank = subtype_rank(subtype);
            bool better;
            if (!best) {
                better = true;
            } else if ((f >= fps - 0.5) != (best_fps >= fps - 0.5)) {
                better = f >= fps - 0.5;  // reaching the wanted rate beats not reaching it
            } else if (f != best_fps) {
                better = f >= fps - 0.5 ? f < best_fps : f > best_fps;
            } else {
                better = rank < best_rank;
            }
            if (better) {
                best.reset();
                best.p = t.p;
                t.p = nullptr;
                best_fps = f;
                best_rank = rank;
            }
        }
        return best && SUCCEEDED(reader->SetCurrentMediaType(kStream, nullptr, best.p));
    }

    void run() {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        int w, h;
        double fps;
        {
            std::lock_guard<std::mutex> hold(lock_);
            w = width_;
            h = height_;
            fps = fps_;
        }
        Com<IMFMediaSource> source;
        Com<IMFSourceReader> reader;
        Com<IMFMediaType> out;
        bool ok = open_reader(link_, source, reader);
        if (ok) {
            std::lock_guard<std::mutex> hold(state_lock_);
            source_ = source.p;
        }
        ok = ok && SUCCEEDED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE)) &&
             SUCCEEDED(reader->SetStreamSelection(kStream, TRUE)) && select_native_type(reader.p, w, h, fps) &&
             SUCCEEDED(MFCreateMediaType(&out)) && SUCCEEDED(out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
             SUCCEEDED(out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12)) &&
             SUCCEEDED(MFSetAttributeSize(out.p, MF_MT_FRAME_SIZE, w, h)) &&
             SUCCEEDED(out->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_0_255)) &&
             SUCCEEDED(reader->SetCurrentMediaType(kStream, nullptr, out.p));
        int stride = 0;
        bool video_range = true;
        if (ok) ok = read_output_type(reader.p, &w, &h, &stride, &video_range);
        std::fprintf(stderr, "[camera] start %ls: %s %dx%d @%.0ffps%s\n", device_name().c_str(),
                     ok ? "streaming" : "FAILED", w, h, fps, ok && video_range ? " (expanding video range)" : "");
        std::fflush(stderr);
        Controls ctl;
        if (ok) {
            ctl.open(source.p);
            controls_ = ctl.mask();
            set_capturing(true);
        }
        signal(ok);
        while (ok && !stopping_) {
            control_tick(ctl, std::chrono::steady_clock::now());
            DWORD index = 0, flags = 0;
            LONGLONG ts = 0;
            Com<IMFSample> sample;
            if (FAILED(reader->ReadSample(kStream, 0, &index, &flags, &ts, &sample)) ||
                (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
                if (!stopping_) std::fprintf(stderr, "[camera] capture ended (device lost or stopped)\n");
                break;
            }
            if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) &&
                !read_output_type(reader.p, &w, &h, &stride, &video_range)) {
                break;
            }
            if (sample) deliver(sample.p, w, h, stride, video_range);
        }
        set_capturing(false);
        ctl.uvc.reset();
        ctl.ks.reset();
        reader.reset();
        {
            std::lock_guard<std::mutex> hold(state_lock_);
            source_ = nullptr;
        }
        if (source) source->Shutdown();
        source.reset();
        {
            std::lock_guard<std::mutex> hold(state_lock_);
            done_ = true;
            state_cv_.notify_all();
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }

    std::wstring device_name() {
        std::lock_guard<std::mutex> hold(g_devices_lock);
        for (const Device& d : g_devices) {
            if (d.link == link_) {
                const int n = MultiByteToWideChar(CP_UTF8, 0, d.name.c_str(), -1, nullptr, 0);
                std::wstring w(static_cast<size_t>(std::max(n, 1)), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, d.name.c_str(), -1, &w[0], n);
                w.resize(wcslen(w.c_str()));
                return w;
            }
        }
        return L"camera";
    }

    bool read_output_type(IMFSourceReader* reader, int* w, int* h, int* stride, bool* video_range) {
        Com<IMFMediaType> t;
        UINT32 tw = 0, th = 0;
        if (FAILED(reader->GetCurrentMediaType(kStream, &t)) ||
            FAILED(MFGetAttributeSize(t.p, MF_MT_FRAME_SIZE, &tw, &th)) || tw == 0 || th == 0) {
            return false;
        }
        *w = static_cast<int>(tw);
        *h = static_cast<int>(th);
        // NV12 rows are top-down; the default stride is the converter's row
        // pitch when it reports one.
        UINT32 s = 0;
        *stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)) && static_cast<INT32>(s) >= *w
                      ? static_cast<int>(s)
                      : *w;
        *video_range = MFGetAttributeUINT32(t.p, MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235) !=
                       MFNominalRange_0_255;
        std::lock_guard<std::mutex> hold(lock_);
        width_ = *w;
        height_ = *h;
        return true;
    }

    // A frame buffer nobody else holds, from a small pool, or a new one.
    std::shared_ptr<Frame> free_frame() {
        std::lock_guard<std::mutex> hold(lock_);
        for (auto& f : pool_) {
            if (f.use_count() == 1) return f;
        }
        pool_.push_back(std::make_shared<Frame>());
        if (pool_.size() > 6) pool_.erase(pool_.begin());  // only reached when consumers keep many alive
        return pool_.back();
    }

    void deliver(IMFSample* sample, int w, int h, int stride, bool video_range) {
        Com<IMFMediaBuffer> buffer;
        if (FAILED(sample->GetBufferByIndex(0, &buffer))) return;
        Com<IMF2DBuffer> twod;
        BYTE* src = nullptr;
        LONG pitch = 0;
        DWORD len = 0;
        bool locked2d = false;
        if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&twod))) && SUCCEEDED(twod->Lock2D(&src, &pitch))) {
            locked2d = true;
        } else if (SUCCEEDED(buffer->Lock(&src, nullptr, &len))) {
            pitch = stride;
            if (static_cast<size_t>(len) < static_cast<size_t>(pitch) * h * 3 / 2) {
                buffer->Unlock();
                return;
            }
        } else {
            return;
        }
        if (pitch < w) {  // bottom-up or malformed: NV12 never is
            if (locked2d) twod->Unlock2D(); else buffer->Unlock();
            return;
        }
        std::shared_ptr<Frame> f = free_frame();
        f->width = w;
        f->height = h;
        f->stride = w;
        f->data.resize(static_cast<size_t>(w) * h * 3 / 2);
        const uint8_t* y = src;
        const uint8_t* c = src + static_cast<size_t>(pitch) * h;
        uint8_t* dy = f->data.data();
        uint8_t* dc = dy + static_cast<size_t>(w) * h;
        const RangeTables& lut = range_tables();
        for (int row = 0; row < h; ++row) {
            const uint8_t* s = y + static_cast<size_t>(row) * pitch;
            uint8_t* d = dy + static_cast<size_t>(row) * w;
            if (video_range) {
                for (int i = 0; i < w; ++i) d[i] = lut.luma[s[i]];
            } else {
                std::memcpy(d, s, static_cast<size_t>(w));
            }
        }
        for (int row = 0; row < h / 2; ++row) {
            const uint8_t* s = c + static_cast<size_t>(row) * pitch;
            uint8_t* d = dc + static_cast<size_t>(row) * w;
            if (video_range) {
                for (int i = 0; i < w; ++i) d[i] = lut.chroma[s[i]];
            } else {
                std::memcpy(d, s, static_cast<size_t>(w));
            }
        }
        if (locked2d) twod->Unlock2D(); else buffer->Unlock();
        f->pts = now_ns();
        f->number = 0;
        {
            std::lock_guard<std::mutex> hold(lock_);
            pending_ = f;  // latest wins: an undrained older frame is dropped
            latest_ = f;
        }
        if (delivered_.fetch_add(1) == 0) {
            std::fprintf(stderr, "[camera] first frame %dx%d\n", w, h);
            std::fflush(stderr);
        }
        std::lock_guard<std::mutex> hold(listener_lock_);
        if (listener_) listener_(f);
    }

    const int device_;
    const std::wstring link_;
    const std::vector<Format> formats_;

    std::mutex lock_;  // geometry and frame slots
    int width_ = 0, height_ = 0;
    double fps_ = 30;
    std::shared_ptr<Frame> pending_, latest_, held_;
    std::vector<std::shared_ptr<Frame>> pool_;
    int64_t acquired_ = 0;
    std::atomic<int64_t> delivered_{0};
    std::mutex listener_lock_;
    std::function<void(const std::shared_ptr<Frame>&)> listener_;

    std::mutex run_lock_;  // start/stop
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::mutex state_lock_;
    std::condition_variable state_cv_;
    bool opened_ = false, failed_ = false, done_ = false;
    IMFMediaSource* source_ = nullptr;  // the capture thread's, for stop's shutdown; under state_lock_

    // Focus and torch requests (under ctl_lock_) and the capture thread's own
    // bookkeeping (capture thread only).
    std::atomic<jint> controls_{0};
    std::atomic<int> focus_state_{0};
    std::mutex ctl_lock_;
    std::condition_variable ctl_cv_;
    bool capturing_ = false;
    int focus_mode_ = kFocusHold;
    uint64_t focus_mode_gen_ = 1, af_req_gen_ = 0, af_done_gen_ = 0, af_cancel_gen_ = 0;
    int af_result_ = -1, af_timeout_ms_ = 3000;
    bool torch_want_ = false, torch_ok_ = false;
    uint64_t torch_gen_ = 0, torch_done_gen_ = 0;
    uint64_t mode_applied_gen_ = 0, af_started_gen_ = 0, af_cancel_seen_ = 0, torch_applied_gen_ = 0;
    bool af_running_ = false, af_moved_ = false, af_seen_search_ = false;
    long last_lens_ = 0;
    std::chrono::steady_clock::time_point af_start_, moved_at_;
};

std::mutex g_sessions_lock;
jlong g_next = 1;  // positive: CameraBridge reserves negative handles for its virtual camera
std::map<jlong, std::shared_ptr<Session>> g_sessions;

std::shared_ptr<Session> session(jlong h) {
    std::lock_guard<std::mutex> hold(g_sessions_lock);
    auto it = g_sessions.find(h);
    return it == g_sessions.end() ? nullptr : it->second;
}

// ---- JNI ------------------------------------------------------------------------

// No camera is reported while the privacy switches deny access: the device
// could not be opened anyway (CameraBridge then offers its virtual camera).
jint JNICALL Cam_deviceCount(JNIEnv*, jclass) {
    if (!camera_allowed()) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) {
            std::fprintf(stderr, "[camera] camera access is off in Windows privacy settings; no camera reported\n");
        }
        return 0;
    }
    refresh_devices(false);
    std::lock_guard<std::mutex> hold(g_devices_lock);
    static std::atomic<int> reported{-1};
    if (reported.exchange(static_cast<int>(g_devices.size())) != static_cast<int>(g_devices.size())) {
        std::fprintf(stderr, "[camera] %zu webcam(s), each offered as a back and a front camera\n", g_devices.size());
    }
    return static_cast<jint>(g_devices.size() * kRolesPerDevice);
}

// The id and name carry the role, so the two cameras of one webcam stay
// distinct (camera2 caches characteristics by id).
jstring device_string(JNIEnv* env, jint index, std::string Device::*field) {
    std::lock_guard<std::mutex> hold(g_devices_lock);
    const int phys = physical_index(index);
    if (phys < 0) return nullptr;
    const bool back = index % kRolesPerDevice == 0;
    const bool id = field == &Device::id;
    const std::string s = g_devices[static_cast<size_t>(phys)].*field +
                          (id ? (back ? "#back" : "#front") : (back ? " (back)" : " (front)"));
    return env->NewStringUTF(s.c_str());
}
jstring JNICALL Cam_deviceId(JNIEnv* env, jclass, jint index) { return device_string(env, index, &Device::id); }
jstring JNICALL Cam_deviceName(JNIEnv* env, jclass, jint index) { return device_string(env, index, &Device::name); }

jint JNICALL Cam_deviceFacing(JNIEnv*, jclass, jint index) {
    std::lock_guard<std::mutex> hold(g_devices_lock);
    if (physical_index(index) < 0) return -1;
    return index % kRolesPerDevice == 0 ? kFacingBack : kFacingFront;
}
// How the webcam's picture sits on the PC's own screen: Windows' capture
// pipeline hands out frames upright for the display they are watched on (an
// integrated camera's mounting rotation is applied by the camera stack), so 0.
// This is not camera2's SENSOR_ORIENTATION: that is relative to the virtual
// phone's natural (portrait) orientation, which CameraBridge.getSensorOrientation
// adds from the display rotation the app is told.
jint JNICALL Cam_sensorOrientation(JNIEnv*, jclass, jint) { return 0; }

// kCtl* bits for camera `index` (probing it once if need be).
jint JNICALL Cam_deviceControls(JNIEnv*, jclass, jint index) {
    Device d;
    return device_at(index, &d) ? d.controls : 0;
}

jint JNICALL Cam_formatCount(JNIEnv*, jclass, jint index) {
    Device d;
    return device_at(index, &d) ? static_cast<jint>(d.formats.size()) : 0;
}
jint JNICALL Cam_formatWidth(JNIEnv*, jclass, jint d, jint f) {
    Format fm;
    return format_at(d, f, &fm) ? fm.width : 0;
}
jint JNICALL Cam_formatHeight(JNIEnv*, jclass, jint d, jint f) {
    Format fm;
    return format_at(d, f, &fm) ? fm.height : 0;
}
jdouble JNICALL Cam_formatMinFps(JNIEnv*, jclass, jint d, jint f) {
    Format fm;
    return format_at(d, f, &fm) ? fm.min_fps : 0;
}
jdouble JNICALL Cam_formatMaxFps(JNIEnv*, jclass, jint d, jint f) {
    Format fm;
    return format_at(d, f, &fm) ? fm.max_fps : 0;
}

jint JNICALL Cam_authStatus(JNIEnv*, jclass) { return camera_allowed() ? kAuthAuthorized : kAuthDenied; }
void JNICALL Cam_requestAuth(JNIEnv*, jclass) {}  // nothing to ask: see the file comment

// Opening proves the device opens (the format probe opens it), so a session is
// only handed out for a camera that can actually stream.
jlong JNICALL Cam_open(JNIEnv*, jclass, jint index) {
    if (!camera_allowed()) return 0;
    Device d;
    if (!device_at(index, &d) || d.formats.empty()) {
        std::fprintf(stderr, "[camera] open(%d): no such camera, or it could not be opened\n", index);
        return 0;
    }
    auto s = std::make_shared<Session>(index, d.link, d.formats);
    std::fprintf(stderr, "[camera] open(%d) %s: %zu sizes\n", index, d.name.c_str(), d.formats.size());
    std::lock_guard<std::mutex> hold(g_sessions_lock);
    const jlong h = g_next++;
    g_sessions[h] = s;
    return h;
}
jboolean JNICALL Cam_configure(JNIEnv*, jclass, jlong h, jint w, jint hgt, jint fps) {
    auto s = session(h);
    return s && s->configure(w, hgt, fps) ? JNI_TRUE : JNI_FALSE;
}
jint JNICALL Cam_width(JNIEnv*, jclass, jlong h) {
    auto s = session(h);
    return s ? s->width() : 0;
}
jint JNICALL Cam_height(JNIEnv*, jclass, jlong h) {
    auto s = session(h);
    return s ? s->height() : 0;
}
jboolean JNICALL Cam_start(JNIEnv*, jclass, jlong h) {
    auto s = session(h);
    return s && s->start() ? JNI_TRUE : JNI_FALSE;
}
void JNICALL Cam_stop(JNIEnv*, jclass, jlong h) {
    if (auto s = session(h)) s->stop();
}
void JNICALL Cam_close(JNIEnv*, jclass, jlong h) {
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> hold(g_sessions_lock);
        auto it = g_sessions.find(h);
        if (it == g_sessions.end()) return;
        s = it->second;
        g_sessions.erase(it);
    }
    s->release();
    s->stop();
}
void JNICALL Cam_setFocusMode(JNIEnv*, jclass, jlong h, jint mode) {
    if (auto s = session(h)) s->set_focus_mode(mode == kFocusContinuous ? kFocusContinuous : kFocusHold);
}
// Blocks for up to timeout_ms (plus a margin): CameraBridge calls it off the
// app's threads.
jint JNICALL Cam_autoFocus(JNIEnv*, jclass, jlong h, jint timeout_ms) {
    auto s = session(h);
    return s ? s->autofocus(timeout_ms) : -1;
}
void JNICALL Cam_cancelAutoFocus(JNIEnv*, jclass, jlong h) {
    if (auto s = session(h)) s->cancel_autofocus();
}
jint JNICALL Cam_focusState(JNIEnv*, jclass, jlong h) {
    auto s = session(h);
    return s ? s->focus_state() : 0;
}
jboolean JNICALL Cam_setTorch(JNIEnv*, jclass, jlong h, jboolean on) {
    auto s = session(h);
    return s && s->set_torch(on == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}

jlong JNICALL Cam_framesDelivered(JNIEnv*, jclass, jlong h) {
    auto s = session(h);
    return s ? s->delivered() : 0;
}

jboolean JNICALL Cam_acquire(JNIEnv*, jclass, jlong h) {
    auto s = session(h);
    return s && s->acquire() ? JNI_TRUE : JNI_FALSE;
}
void JNICALL Cam_release(JNIEnv*, jclass, jlong h) {
    if (auto s = session(h)) s->release();
}

// Reachable bytes of a plane, as Android sizes an Image.Plane buffer: up to the
// last sample of the last row, so the V view (one byte into the chroma plane)
// never runs past the frame.
jobject JNICALL Cam_planeBuffer(JNIEnv* env, jclass, jlong h, jint plane) {
    auto s = session(h);
    auto f = s ? s->held() : nullptr;
    if (!f || plane < 0 || plane > 2) return nullptr;
    uint8_t* base = f->data.data();
    const size_t luma = static_cast<size_t>(f->stride) * f->height;
    if (plane == 0) return env->NewDirectByteBuffer(base, static_cast<jlong>(luma));
    const size_t rows = static_cast<size_t>(f->height) / 2, pairs = static_cast<size_t>(f->width) / 2;
    if (rows == 0 || pairs == 0) return nullptr;
    const size_t extent = static_cast<size_t>(f->stride) * (rows - 1) + 2 * (pairs - 1) + 1;
    // The buffer stays valid until release(): the session's held reference
    // keeps the frame, which is CameraBridge's Image.close() contract.
    return env->NewDirectByteBuffer(base + luma + (plane == 2 ? 1 : 0), static_cast<jlong>(extent));
}
jint JNICALL Cam_rowStride(JNIEnv*, jclass, jlong h, jint plane) {
    auto s = session(h);
    auto f = s ? s->held() : nullptr;
    return f && plane >= 0 && plane <= 2 ? f->stride : 0;
}
jint JNICALL Cam_pixelStride(JNIEnv*, jclass, jlong h, jint plane) {
    auto s = session(h);
    auto f = s ? s->held() : nullptr;
    if (!f || plane < 0 || plane > 2) return 0;
    return plane == 0 ? 1 : 2;
}
jlong JNICALL Cam_frameTimestamp(JNIEnv*, jclass, jlong h) {
    auto s = session(h);
    auto f = s ? s->held() : nullptr;
    return f ? f->pts : 0;
}
jlong JNICALL Cam_frameNumber(JNIEnv*, jclass, jlong h) {
    auto s = session(h);
    auto f = s ? s->held() : nullptr;
    return f ? f->number : 0;
}

jlong JNICALL Cam_latestTimestamp(JNIEnv*, jclass, jlong h) {
    auto s = session(h);
    auto f = s ? s->latest() : nullptr;
    return f ? std::max<jlong>(f->pts, 1) : 0;
}

// Full-range BT.601 (JFIF) NV12 -> RGB in 8.8 fixed point, row 0 = top row;
// the same math as upstream's latest_frame_convert for '420f'.
template <bool kArgb>
void convert(const Frame& f, void* dst) {
    const uint8_t* yb = f.data.data();
    const uint8_t* cb = yb + static_cast<size_t>(f.stride) * f.height;
    for (int row = 0; row < f.height; ++row) {
        const uint8_t* yp = yb + static_cast<size_t>(row) * f.stride;
        const uint8_t* cp = cb + static_cast<size_t>(row >> 1) * f.stride;
        for (int col = 0; col < f.width; ++col) {
            const int y = yp[col];
            const int d = cp[col & ~1] - 128;  // Cb first: NV12
            const int e = cp[(col & ~1) + 1] - 128;
            const int r = std::clamp(y + ((359 * e + 128) >> 8), 0, 255);
            const int g = std::clamp(y - ((88 * d + 183 * e + 128) >> 8), 0, 255);
            const int b = std::clamp(y + ((454 * d + 128) >> 8), 0, 255);
            const size_t i = static_cast<size_t>(row) * f.width + col;
            if constexpr (kArgb) {
                static_cast<uint32_t*>(dst)[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
            } else {
                uint8_t* o = static_cast<uint8_t*>(dst) + i * 4;
                o[0] = static_cast<uint8_t>(r);
                o[1] = static_cast<uint8_t>(g);
                o[2] = static_cast<uint8_t>(b);
                o[3] = 0xFF;
            }
        }
    }
}

void set_dims(JNIEnv* env, jintArray dims, const Frame& f) {
    if (!dims || env->GetArrayLength(dims) < 2) return;
    const jint d[2] = {f.width, f.height};
    env->SetIntArrayRegion(dims, 0, 2, d);
}

// dims is set whenever a frame exists, so a caller whose buffer is too small
// can grow it and retry.
jlong JNICALL Cam_copyLatestRgba(JNIEnv* env, jclass, jlong h, jobject dst, jintArray dims) {
    auto s = session(h);
    auto f = s ? s->latest() : nullptr;
    if (!f || !dst) return 0;
    set_dims(env, dims, *f);
    void* addr = env->GetDirectBufferAddress(dst);
    const jlong cap = env->GetDirectBufferCapacity(dst);
    if (!addr || cap < static_cast<jlong>(f->width) * f->height * 4) return 0;
    convert<false>(*f, addr);
    return std::max<jlong>(f->pts, 1);
}

// The frame is held by reference before the array is pinned, so nothing inside
// the critical region can block or allocate.
jlong JNICALL Cam_copyLatestArgb(JNIEnv* env, jclass, jlong h, jintArray dst, jintArray dims) {
    auto s = session(h);
    auto f = s ? s->latest() : nullptr;
    if (!f || !dst) return 0;
    set_dims(env, dims, *f);
    if (env->GetArrayLength(dst) < static_cast<jsize>(f->width) * f->height) return 0;
    void* out = env->GetPrimitiveArrayCritical(dst, nullptr);
    if (!out) return 0;
    convert<true>(*f, out);
    env->ReleasePrimitiveArrayCritical(dst, out, 0);
    return std::max<jlong>(f->pts, 1);
}


// ---- the camera screen (CameraCaptureHandler: ACTION_IMAGE_CAPTURE) ------------
//
// What the camera app's capture activity is on a phone: a live picture from the
// webcam, Take photo, then Use photo or Retake; Cancel at any time. A PC often
// has no camera, or one that faces the user, so a picture file can be chosen
// instead (and is shown for Use photo the same way). The window is owned by the
// app's window, which is disabled while it is up, as under a modal dialog; it
// runs on a thread of its own and answers CameraCaptureHandler.macapkOnCaptured
// with the JPEG, or null for a cancel. The picture is the webcam's frame as it
// is, not mirrored: what a phone's back camera gives, and text stays readable.

constexpr UINT kMsgStarted = WM_APP + 1;  // wParam: 1 when the camera streams
constexpr UINT_PTR kTimerFrame = 1;
constexpr int kBtnTake = 101, kBtnUse = 102, kBtnRetake = 103, kBtnCancel = 104, kBtnFile = 105, kBtnSwitch = 106;
constexpr int kBarDip = 64, kBtnHeightDip = 32, kBtnGapDip = 8, kBtnPadDip = 24;

std::atomic<bool> g_capture_open{false};

enum CaptureText {
    kTxtTitle, kTxtTake, kTxtUse, kTxtRetake, kTxtCancel, kTxtFile, kTxtSwitch, kTxtStarting, kTxtNoCamera,
    kTxtDenied, kTxtFailed, kTxtSaveFailed, kTxtPictures
};

const wchar_t* capture_text(int key) {
    static const bool he = ui_hebrew();
    static const wchar_t* const kEn[] = {
        L"Camera",
        L"Take photo",
        L"Use photo",
        L"Retake",
        L"Cancel",
        L"Choose a file…",
        L"Switch camera",
        L"Starting the camera…",
        L"No camera was found. You can choose a picture file instead.",
        (L"Camera access is off in Windows settings (Privacy & security > Camera). You can choose a picture file "
         L"instead."),
        L"The camera could not be started. Another program may be using it.",
        L"The picture could not be saved.",
        L"Pictures",
    };
    static const wchar_t* const kHe[] = {
        L"מצלמה",
        L"צילום",
        L"שימוש בתמונה",
        L"צילום חוזר",
        L"ביטול",
        L"בחירת קובץ…",
        L"החלפת מצלמה",
        L"מפעיל את המצלמה…",
        (L"לא נמצאה מצלמה. אפשר "
         L"לבחור קובץ תמונה "
         L"במקום."),
        (L"הגישה למצלמה כבויה "
         L"בהגדרות Windows (פרטיות "
         L"ואבטחה > מצלמה). אפשר "
         L"לבחור קובץ תמונה "
         L"במקום."),
        (L"לא ניתן להפעיל את "
         L"המצלמה. ייתכן שתוכנה "
         L"אחרת משתמשת בה."),
        (L"לא ניתן לשמור את "
         L"התמונה."),
        L"תמונות",
    };
    return (he ? kHe : kEn)[key];
}

// A picture as 32-bit BGRA rows, top first (a frame of the webcam, or a file).
struct Picture {
    std::vector<uint32_t> px;
    int width = 0, height = 0;
    bool empty() const { return px.empty(); }
};

struct CaptureUi {
    HWND hwnd = nullptr, owner = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr;
    std::map<int, HWND> buttons;
    int webcams = 0, webcam = 0;  // the physical webcam shown
    std::shared_ptr<Session> session;
    enum class State { kStarting, kLive, kNoCamera, kDenied, kFailed, kTaken } state = State::kStarting;
    Picture live, taken;
    int64_t shown_pts = 0;
    std::vector<uint8_t> jpeg;  // the answer
    bool done = false;
};

int dip(const CaptureUi& ui, int v) { return MulDiv(v, static_cast<int>(ui.dpi), 96); }

UINT dpi_of(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    static auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    const UINT d = fn && hwnd ? fn(hwnd) : 0;
    return d ? d : 96;
}

// Starts the session for webcam `ui.webcam` off the window's thread (opening a
// device can take seconds) and posts kMsgStarted.
void capture_start(CaptureUi& ui) {
    if (ui.session) ui.session->stop();
    ui.session.reset();
    ui.live = Picture{};
    ui.shown_pts = 0;
    ui.state = CaptureUi::State::kStarting;
    Device d;
    if (!device_at(ui.webcam * kRolesPerDevice, &d) || d.formats.empty()) {
        ui.state = CaptureUi::State::kFailed;
        return;
    }
    auto s = std::make_shared<Session>(ui.webcam * kRolesPerDevice, d.link, d.formats);
    // The largest picture the webcam has: a document or a cheque wants detail.
    s->configure(4096, 3072, 30);
    ui.session = s;
    std::fprintf(stderr, "[camera] capture screen: %s at %dx%d\n", d.name.c_str(), s->width(), s->height());
    const HWND hwnd = ui.hwnd;
    std::thread([s, hwnd] {
        const bool ok = s->start();
        PostMessageW(hwnd, kMsgStarted, ok ? 1 : 0, reinterpret_cast<LPARAM>(s.get()));
    }).detach();
}

void capture_layout(CaptureUi& ui) {
    for (auto& [id, b] : ui.buttons) DestroyWindow(b);
    ui.buttons.clear();
    std::vector<int> ids;  // left to right in English; the first is the default action
    switch (ui.state) {
        case CaptureUi::State::kTaken: ids = {kBtnUse, kBtnRetake, kBtnCancel}; break;
        case CaptureUi::State::kStarting:
        case CaptureUi::State::kLive:
            ids = {kBtnTake};
            if (ui.webcams > 1) ids.push_back(kBtnSwitch);
            ids.push_back(kBtnFile);
            ids.push_back(kBtnCancel);
            break;
        default: ids = {kBtnFile, kBtnCancel}; break;
    }
    const auto label = [](int id) {
        switch (id) {
            case kBtnTake: return capture_text(kTxtTake);
            case kBtnUse: return capture_text(kTxtUse);
            case kBtnRetake: return capture_text(kTxtRetake);
            case kBtnFile: return capture_text(kTxtFile);
            case kBtnSwitch: return capture_text(kTxtSwitch);
            default: return capture_text(kTxtCancel);
        }
    };
    RECT c{};
    GetClientRect(ui.hwnd, &c);
    HDC dc = GetDC(ui.hwnd);
    HGDIOBJ old = SelectObject(dc, ui.font);
    std::vector<int> widths;
    int total = 0;
    for (int id : ids) {
        SIZE sz{};
        const wchar_t* t = label(id);
        GetTextExtentPoint32W(dc, t, static_cast<int>(wcslen(t)), &sz);
        const int w = std::max(static_cast<int>(sz.cx) + 2 * dip(ui, kBtnPadDip), dip(ui, 96));
        widths.push_back(w);
        total += w;
    }
    SelectObject(dc, old);
    ReleaseDC(ui.hwnd, dc);
    total += dip(ui, kBtnGapDip) * static_cast<int>(ids.size() - 1);
    const int h = dip(ui, kBtnHeightDip);
    const int y = c.bottom - dip(ui, kBarDip) + (dip(ui, kBarDip) - h) / 2;
    int x = (c.right - total) / 2;
    const bool rtl = ui_hebrew();
    for (size_t i = 0; i < ids.size(); ++i) {
        // Right to left in Hebrew: the default action stays at the reading start.
        const size_t k = rtl ? ids.size() - 1 - i : i;
        const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | (k == 0 ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON);
        HWND b = CreateWindowExW(rtl ? WS_EX_RTLREADING : 0, L"BUTTON", label(ids[k]), style, x, y, widths[k], h,
                                 ui.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids[k])),
                                 GetModuleHandleW(nullptr), nullptr);
        SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
        if (ids[k] == kBtnTake) EnableWindow(b, ui.state == CaptureUi::State::kLive && !ui.live.empty());
        ui.buttons[ids[k]] = b;
        x += widths[k] + dip(ui, kBtnGapDip);
    }
    if (!ids.empty()) SetFocus(ui.buttons[ids[0]]);
    InvalidateRect(ui.hwnd, nullptr, FALSE);
}

// The newest webcam frame into ui.live; true when it is a new one.
bool capture_pull(CaptureUi& ui) {
    auto f = ui.session ? ui.session->latest() : nullptr;
    if (!f || f->pts == ui.shown_pts) return false;
    ui.shown_pts = f->pts;
    ui.live.width = f->width;
    ui.live.height = f->height;
    ui.live.px.resize(static_cast<size_t>(f->width) * f->height);
    convert<true>(*f, ui.live.px.data());
    return true;
}

void capture_paint(CaptureUi& ui) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(ui.hwnd, &ps);
    RECT c{};
    GetClientRect(ui.hwnd, &c);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, std::max(1L, c.right), std::max(1L, c.bottom));
    HGDIOBJ oldbmp = SelectObject(dc, bmp);
    RECT view{0, 0, c.right, c.bottom - dip(ui, kBarDip)};
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &view, black);
    DeleteObject(black);
    RECT bar{0, view.bottom, c.right, c.bottom};
    FillRect(dc, &bar, GetSysColorBrush(COLOR_BTNFACE));
    const Picture& pic = ui.state == CaptureUi::State::kTaken ? ui.taken : ui.live;
    const int vw = view.right - view.left, vh = view.bottom - view.top;
    if (!pic.empty() && vw > 0 && vh > 0) {
        // The largest box of the picture's aspect, centred.
        int w = vw, h = static_cast<int>(static_cast<long long>(vw) * pic.height / pic.width);
        if (h > vh) {
            h = vh;
            w = static_cast<int>(static_cast<long long>(vh) * pic.width / pic.height);
        }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = pic.width;
        bi.bmiHeader.biHeight = -pic.height;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, (vw - w) / 2, (vh - h) / 2, w, h, 0, 0, pic.width, pic.height, pic.px.data(), &bi,
                      DIB_RGB_COLORS, SRCCOPY);
    } else {
        int key = -1;
        switch (ui.state) {
            case CaptureUi::State::kStarting:
            case CaptureUi::State::kLive: key = kTxtStarting; break;
            case CaptureUi::State::kNoCamera: key = kTxtNoCamera; break;
            case CaptureUi::State::kDenied: key = kTxtDenied; break;
            case CaptureUi::State::kFailed: key = kTxtFailed; break;
            default: break;
        }
        if (key >= 0) {
            HGDIOBJ oldfont = SelectObject(dc, ui.font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(230, 230, 230));
            RECT t = view;
            InflateRect(&t, -dip(ui, 32), 0);
            const UINT rtl = ui_hebrew() ? DT_RTLREADING : 0;
            RECT m = t;
            DrawTextW(dc, capture_text(key), -1, &m, DT_CENTER | DT_WORDBREAK | DT_CALCRECT | rtl);
            const int th = m.bottom - m.top;
            t.top = view.top + (vh - th) / 2;
            t.bottom = t.top + th;
            DrawTextW(dc, capture_text(key), -1, &t, DT_CENTER | DT_WORDBREAK | rtl);
            SelectObject(dc, oldfont);
        }
    }
    BitBlt(wdc, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldbmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(ui.hwnd, &ps);
}

// The picture as a JPEG (WIC), quality 0.92. Empty on failure.
std::vector<uint8_t> encode_jpeg(const Picture& pic) {
    std::vector<uint8_t> out;
    Com<IWICImagingFactory> wic;
    Com<IWICBitmap> bitmap;
    Com<IStream> stream;
    Com<IWICBitmapEncoder> encoder;
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> props;
    Com<IWICFormatConverter> bgr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateBitmapFromMemory(static_cast<UINT>(pic.width), static_cast<UINT>(pic.height),
                                           GUID_WICPixelFormat32bppBGRA, static_cast<UINT>(pic.width) * 4,
                                           static_cast<UINT>(pic.px.size() * 4),
                                           reinterpret_cast<BYTE*>(const_cast<uint32_t*>(pic.px.data())),
                                           &bitmap)) ||
        FAILED(wic->CreateFormatConverter(&bgr)) ||
        FAILED(bgr->Initialize(bitmap.p, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                               WICBitmapPaletteTypeCustom)) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.p, WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, &props))) {
        return out;
    }
    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_R4;
    value.fltVal = 0.92f;
    props->Write(1, &option, &value);
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->Initialize(props.p)) ||
        FAILED(frame->SetSize(static_cast<UINT>(pic.width), static_cast<UINT>(pic.height))) ||
        FAILED(frame->SetPixelFormat(&format)) || FAILED(frame->WriteSource(bgr.p, nullptr)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        return out;
    }
    HGLOBAL mem = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.p, &mem)) || !mem) return out;
    STATSTG st{};
    if (FAILED(stream->Stat(&st, STATFLAG_NONAME))) return out;
    const void* bytes = GlobalLock(mem);
    if (bytes) {
        const auto* b = static_cast<const uint8_t*>(bytes);
        out.assign(b, b + static_cast<size_t>(st.cbSize.QuadPart));
        GlobalUnlock(mem);
    }
    return out;
}

// A picture file the user chooses (any format WIC decodes), as BGRA.
bool choose_picture(HWND owner, Picture* out) {
    Com<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return false;
    }
    const COMDLG_FILTERSPEC filter[] = {
        {capture_text(kTxtPictures), L"*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff;*.heic;*.webp"}};
    dialog->SetFileTypes(1, filter);
    DWORD opts = 0;
    dialog->GetOptions(&opts);
    dialog->SetOptions(opts | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    if (FAILED(dialog->Show(owner))) return false;
    Com<IShellItem> item;
    wchar_t* path = nullptr;
    if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return false;
    Com<IWICImagingFactory> wic;
    Com<IWICBitmapDecoder> decoder;
    Com<IWICBitmapFrameDecode> frame;
    Com<IWICFormatConverter> bgra;
    UINT w = 0, h = 0;
    const bool ok =
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                 &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&bgra)) &&
        SUCCEEDED(bgra->Initialize(frame.p, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(bgra->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 16384 && h <= 16384;
    CoTaskMemFree(path);
    if (!ok) return false;
    out->width = static_cast<int>(w);
    out->height = static_cast<int>(h);
    out->px.assign(static_cast<size_t>(w) * h, 0);
    if (FAILED(bgra->CopyPixels(nullptr, w * 4, w * h * 4, reinterpret_cast<BYTE*>(out->px.data())))) return false;
    for (uint32_t& p : out->px) p |= 0xFF000000u;  // a JPEG has no alpha: transparency becomes the colour as is
    return true;
}

void capture_command(CaptureUi& ui, int id) {
    switch (id) {
        case kBtnTake:
            if (ui.state != CaptureUi::State::kLive || ui.live.empty()) return;
            ui.taken = ui.live;
            ui.state = CaptureUi::State::kTaken;
            capture_layout(ui);
            return;
        case kBtnRetake:
            ui.taken = Picture{};
            if (ui.session && ui.session->running()) {
                ui.state = CaptureUi::State::kLive;  // Take is enabled again with the next frame
            } else if (ui.webcams > 0) {
                capture_start(ui);
            } else {
                ui.state = CaptureUi::State::kNoCamera;
            }
            capture_layout(ui);
            return;
        case kBtnUse: {
            if (ui.taken.empty()) return;
            ui.jpeg = encode_jpeg(ui.taken);
            if (ui.jpeg.empty()) {
                MessageBoxW(ui.hwnd, capture_text(kTxtSaveFailed), capture_text(kTxtTitle),
                            MB_OK | MB_ICONERROR | (ui_hebrew() ? MB_RTLREADING | MB_RIGHT : 0));
                return;
            }
            ui.done = true;
            DestroyWindow(ui.hwnd);
            return;
        }
        case kBtnFile: {
            Picture pic;
            if (!choose_picture(ui.hwnd, &pic)) return;
            ui.taken = std::move(pic);
            ui.state = CaptureUi::State::kTaken;
            capture_layout(ui);
            return;
        }
        case kBtnSwitch:
            if (ui.webcams < 2) return;
            ui.webcam = (ui.webcam + 1) % ui.webcams;
            capture_start(ui);
            capture_layout(ui);
            return;
        case kBtnCancel: DestroyWindow(ui.hwnd); return;
        default: return;
    }
}

LRESULT CALLBACK capture_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ui = reinterpret_cast<CaptureUi*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case WM_COMMAND:
            if (ui && HIWORD(wp) == BN_CLICKED) capture_command(*ui, LOWORD(wp));
            return 0;
        case WM_TIMER:
            if (ui && wp == kTimerFrame && ui->state == CaptureUi::State::kLive && capture_pull(*ui)) {
                if (ui->buttons.count(kBtnTake)) EnableWindow(ui->buttons[kBtnTake], TRUE);
                RECT c{};
                GetClientRect(hwnd, &c);
                c.bottom -= dip(*ui, kBarDip);
                InvalidateRect(hwnd, &c, FALSE);
            }
            return 0;
        case kMsgStarted:
            // A start that a switch has since replaced says nothing.
            if (ui && ui->session && reinterpret_cast<LPARAM>(ui->session.get()) == lp &&
                ui->state == CaptureUi::State::kStarting) {
                ui->state = wp ? CaptureUi::State::kLive : CaptureUi::State::kFailed;
                if (!wp) std::fprintf(stderr, "[camera] capture screen: the camera did not start\n");
                capture_layout(*ui);
            }
            return 0;
        case WM_SIZE:
            if (ui && ui->hwnd) capture_layout(*ui);  // not during CreateWindowEx: the buttons come after
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            const UINT d = dpi_of(hwnd);
            mm->ptMinTrackSize = POINT{MulDiv(420, static_cast<int>(d), 96), MulDiv(320, static_cast<int>(d), 96)};
            return 0;
        }
        case WM_DPICHANGED:
            if (ui) {
                ui->dpi = HIWORD(wp);
                const RECT* r = reinterpret_cast<const RECT*>(lp);
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
            if (ui) {
                capture_paint(*ui);
                return 0;
            }
            break;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kTimerFrame);
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Shows the screen over `owner` (may be null) and runs it to the end, on the
// calling thread: the JPEG the user chose to use, or empty for a cancel. COM
// must be initialized single-threaded on that thread (the file dialog's).
std::vector<uint8_t> run_capture_screen(HWND owner) {
    static std::once_flag registered;
    std::call_once(registered, [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = capture_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
        wc.lpszClassName = L"WinApkCameraCapture";
        RegisterClassExW(&wc);
    });
    CaptureUi ui;
    ui.owner = owner;
    ui.dpi = dpi_of(ui.owner);
    ui.font = CreateFontW(-MulDiv(10, static_cast<int>(ui.dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH, L"Segoe UI");
    // Over the app's window, about its height, a 4:3 picture plus the bar.
    RECT owner_rect{0, 0, 0, 0};
    if (ui.owner) GetWindowRect(ui.owner, &owner_rect);
    const int ch = std::clamp(static_cast<int>(owner_rect.bottom - owner_rect.top) - dip(ui, 80), dip(ui, 360),
                              dip(ui, 640));
    const int cw = std::max((ch - dip(ui, kBarDip)) * 4 / 3, dip(ui, 480));
    RECT r{0, 0, cw, ch};
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (ui.owner) {
        x = (owner_rect.left + owner_rect.right - ww) / 2;
        y = std::max(static_cast<int>(owner_rect.top), static_cast<int>((owner_rect.top + owner_rect.bottom - wh) / 2));
        HMONITOR mon = MonitorFromWindow(ui.owner, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(mon, &mi)) {
            x = std::clamp(x, static_cast<int>(mi.rcWork.left),
                           std::max(static_cast<int>(mi.rcWork.left), static_cast<int>(mi.rcWork.right) - ww));
            y = std::clamp(y, static_cast<int>(mi.rcWork.top),
                           std::max(static_cast<int>(mi.rcWork.top), static_cast<int>(mi.rcWork.bottom) - wh));
        }
    }
    refresh_devices(true);
    {
        std::lock_guard<std::mutex> hold(g_devices_lock);
        ui.webcams = static_cast<int>(g_devices.size());
    }
    ui.hwnd = CreateWindowExW(0, L"WinApkCameraCapture", capture_text(kTxtTitle), WS_OVERLAPPEDWINDOW, x, y, ww, wh,
                              ui.owner, nullptr, GetModuleHandleW(nullptr), &ui);
    if (ui.hwnd) {
        if (ui.owner) EnableWindow(ui.owner, FALSE);
        if (!camera_allowed()) {
            ui.state = CaptureUi::State::kDenied;
        } else if (ui.webcams == 0) {
            ui.state = CaptureUi::State::kNoCamera;
        } else {
            capture_start(ui);
        }
        std::fprintf(stderr, "[camera] capture screen: %d webcam(s)%s\n", ui.webcams,
                     ui.state == CaptureUi::State::kDenied ? ", access off in Windows settings" : "");
        capture_layout(ui);
        SetTimer(ui.hwnd, kTimerFrame, 33, nullptr);
        ShowWindow(ui.hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(ui.hwnd);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
                DestroyWindow(ui.hwnd);
                continue;
            }
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
                // The focused button, as in a dialog; else the default action.
                const HWND focus = GetFocus();
                int id = ui.state == CaptureUi::State::kTaken ? kBtnUse : kBtnTake;
                for (const auto& [bid, b] : ui.buttons) {
                    if (b == focus) id = bid;
                }
                capture_command(ui, id);
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (ui.owner) {
            EnableWindow(ui.owner, TRUE);
            SetForegroundWindow(ui.owner);
        }
    }
    if (ui.session) ui.session->stop();
    ui.session.reset();
    DeleteObject(ui.font);
    std::fprintf(stderr, "[camera] capture screen: %s\n",
                 ui.done ? (std::to_string(ui.jpeg.size()) + " bytes").c_str() : "cancelled");
    return ui.done ? std::move(ui.jpeg) : std::vector<uint8_t>{};
}

void capture_thread(JavaVM* vm, jclass handler, jlong id) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const std::vector<uint8_t> jpeg = run_capture_screen(static_cast<HWND>(window_handle()));
    if (SUCCEEDED(co)) CoUninitialize();
    JNIEnv* env = nullptr;
    if (vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), nullptr) == JNI_OK) {
        jbyteArray bytes = nullptr;
        if (!jpeg.empty()) {
            bytes = env->NewByteArray(static_cast<jsize>(jpeg.size()));
            if (bytes) {
                env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(jpeg.size()),
                                        reinterpret_cast<const jbyte*>(jpeg.data()));
            }
        }
        jmethodID done = env->GetStaticMethodID(handler, "macapkOnCaptured", "(J[B)V");
        if (done) env->CallStaticVoidMethod(handler, done, id, bytes);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteGlobalRef(handler);
        vm->DetachCurrentThread();
    }
    g_capture_open = false;
}

jboolean JNICALL Capture_available(JNIEnv*, jclass) { return JNI_TRUE; }

// One screen at a time: a second request while one is up is a cancel.
jboolean JNICALL Capture_capture(JNIEnv* env, jclass cls, jlong id) {
    if (g_capture_open.exchange(true)) return JNI_FALSE;
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK) {
        g_capture_open = false;
        return JNI_FALSE;
    }
    auto handler = static_cast<jclass>(env->NewGlobalRef(cls));
    std::thread(capture_thread, vm, handler, id).detach();
    return JNI_TRUE;
}

constexpr const char* kCaptureCls = "com.macapk.loader.CameraCaptureHandler";

constexpr const char* kCls = "com.macapk.loader.CameraBridge";

const HostMethod kMethods[] = {
    WINAPK_HOST_NATIVE(kCls, "nativeDeviceCount", "()I", Cam_deviceCount),
    WINAPK_HOST_NATIVE(kCls, "nativeDeviceId", "(I)Ljava/lang/String;", Cam_deviceId),
    WINAPK_HOST_NATIVE(kCls, "nativeDeviceName", "(I)Ljava/lang/String;", Cam_deviceName),
    WINAPK_HOST_NATIVE(kCls, "nativeDeviceFacing", "(I)I", Cam_deviceFacing),
    WINAPK_HOST_NATIVE(kCls, "nativeDeviceSensorOrientation", "(I)I", Cam_sensorOrientation),
    WINAPK_HOST_NATIVE(kCls, "nativeDeviceControls", "(I)I", Cam_deviceControls),
    WINAPK_HOST_NATIVE(kCls, "nativeSetFocusMode", "(JI)V", Cam_setFocusMode),
    WINAPK_HOST_NATIVE(kCls, "nativeAutoFocus", "(JI)I", Cam_autoFocus),
    WINAPK_HOST_NATIVE(kCls, "nativeCancelAutoFocus", "(J)V", Cam_cancelAutoFocus),
    WINAPK_HOST_NATIVE(kCls, "nativeFocusState", "(J)I", Cam_focusState),
    WINAPK_HOST_NATIVE(kCls, "nativeSetTorch", "(JZ)Z", Cam_setTorch),
    WINAPK_HOST_NATIVE(kCls, "nativeFormatCount", "(I)I", Cam_formatCount),
    WINAPK_HOST_NATIVE(kCls, "nativeFormatWidth", "(II)I", Cam_formatWidth),
    WINAPK_HOST_NATIVE(kCls, "nativeFormatHeight", "(II)I", Cam_formatHeight),
    WINAPK_HOST_NATIVE(kCls, "nativeFormatMinFps", "(II)D", Cam_formatMinFps),
    WINAPK_HOST_NATIVE(kCls, "nativeFormatMaxFps", "(II)D", Cam_formatMaxFps),
    WINAPK_HOST_NATIVE(kCls, "nativeAuthStatus", "()I", Cam_authStatus),
    WINAPK_HOST_NATIVE(kCls, "nativeRequestAuth", "()V", Cam_requestAuth),
    WINAPK_HOST_NATIVE(kCls, "nativeOpen", "(I)J", Cam_open),
    WINAPK_HOST_NATIVE(kCls, "nativeConfigure", "(JIII)Z", Cam_configure),
    WINAPK_HOST_NATIVE(kCls, "nativeWidth", "(J)I", Cam_width),
    WINAPK_HOST_NATIVE(kCls, "nativeHeight", "(J)I", Cam_height),
    WINAPK_HOST_NATIVE(kCls, "nativeStart", "(J)Z", Cam_start),
    WINAPK_HOST_NATIVE(kCls, "nativeStop", "(J)V", Cam_stop),
    WINAPK_HOST_NATIVE(kCls, "nativeClose", "(J)V", Cam_close),
    WINAPK_HOST_NATIVE(kCls, "nativeFramesDelivered", "(J)J", Cam_framesDelivered),
    WINAPK_HOST_NATIVE(kCls, "nativeAcquire", "(J)Z", Cam_acquire),
    WINAPK_HOST_NATIVE(kCls, "nativeRelease", "(J)V", Cam_release),
    WINAPK_HOST_NATIVE(kCls, "nativePlaneBuffer", "(JI)Ljava/nio/ByteBuffer;", Cam_planeBuffer),
    WINAPK_HOST_NATIVE(kCls, "nativeRowStride", "(JI)I", Cam_rowStride),
    WINAPK_HOST_NATIVE(kCls, "nativePixelStride", "(JI)I", Cam_pixelStride),
    WINAPK_HOST_NATIVE(kCls, "nativeFrameTimestamp", "(J)J", Cam_frameTimestamp),
    WINAPK_HOST_NATIVE(kCls, "nativeFrameNumber", "(J)J", Cam_frameNumber),
    WINAPK_HOST_NATIVE(kCls, "nativeLatestTimestamp", "(J)J", Cam_latestTimestamp),
    WINAPK_HOST_NATIVE(kCls, "nativeCopyLatestRgba", "(JLjava/nio/ByteBuffer;[I)J", Cam_copyLatestRgba),
    WINAPK_HOST_NATIVE(kCls, "nativeCopyLatestArgb", "(J[I[I)J", Cam_copyLatestArgb),
    WINAPK_HOST_NATIVE(kCaptureCls, "nativeAvailable", "()Z", Capture_available),
    WINAPK_HOST_NATIVE(kCaptureCls, "nativeCapture", "(J)Z", Capture_capture),
};

}  // namespace

// ---- camera_host.h: the same cameras for native code ------------------------------

namespace camera {

namespace {

class SessionStream : public Stream {
public:
    explicit SessionStream(std::shared_ptr<Session> s) : s_(std::move(s)) {}
    ~SessionStream() override {
        s_->set_listener(nullptr);
        s_->stop();
    }
    bool configure(int width, int height, int fps) override { return s_->configure(width, height, fps); }
    int width() override { return s_->width(); }
    int height() override { return s_->height(); }
    bool start() override { return s_->start(); }
    void stop() override { s_->stop(); }
    void set_listener(std::function<void(const Picture&)> listener) override {
        if (!listener) {
            s_->set_listener(nullptr);
            return;
        }
        s_->set_listener([listener = std::move(listener)](const std::shared_ptr<Frame>& f) {
            Picture p;
            p.nv12 = f->data.data();
            p.width = f->width;
            p.height = f->height;
            p.stride = f->stride;
            p.timestamp_ns = f->pts;
            p.hold = f;
            listener(p);
        });
    }
    void set_continuous_focus(bool on) override { s_->set_focus_mode(on ? kFocusContinuous : kFocusHold); }
    bool set_torch(bool on) override { return s_->set_torch(on); }
    int focus_state() override { return s_->focus_state(); }

private:
    std::shared_ptr<Session> s_;
};

}  // namespace

bool allowed() { return camera_allowed(); }

// AudioRecord's way of asking (native_audio_input.cpp's record_audio_permitted), for CAMERA.
bool permitted() {
    using GetVms = jint(JNICALL*)(JavaVM**, jsize, jsize*);
    HMODULE jvm = GetModuleHandleW(L"jvm.dll");
    auto get_vms = jvm ? reinterpret_cast<GetVms>(GetProcAddress(jvm, "JNI_GetCreatedJavaVMs")) : nullptr;
    JavaVM* vm = nullptr;
    jsize n = 0;
    if (!get_vms || get_vms(&vm, 1, &n) != JNI_OK || n < 1 || !vm) return true;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        JavaVMAttachArgs args{JNI_VERSION_1_6, const_cast<char*>("winapk-camera-permission"), nullptr};
        if (vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), &args) != JNI_OK) return true;
    }
    bool held = true;
    jclass store = env->FindClass("com/macapk/loader/SpecialAccessStore");
    jmethodID holds =
        store ? env->GetStaticMethodID(store, "holds", "(Ljava/lang/String;)Ljava/lang/Boolean;") : nullptr;
    jstring perm = holds ? env->NewStringUTF("android.permission.CAMERA") : nullptr;
    jobject answer = perm ? env->CallStaticObjectMethod(store, holds, perm) : nullptr;
    if (answer && !env->ExceptionCheck()) {
        jclass boolean = env->FindClass("java/lang/Boolean");
        jmethodID value = boolean ? env->GetMethodID(boolean, "booleanValue", "()Z") : nullptr;
        if (value) held = env->CallBooleanMethod(answer, value) != JNI_FALSE;
        if (boolean) env->DeleteLocalRef(boolean);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (answer) env->DeleteLocalRef(answer);
    if (perm) env->DeleteLocalRef(perm);
    if (store) env->DeleteLocalRef(store);
    return held;
}

int count() {
    if (!camera_allowed()) return 0;
    refresh_devices(false);
    std::lock_guard<std::mutex> hold(g_devices_lock);
    return static_cast<int>(g_devices.size() * kRolesPerDevice);
}

bool info(int index, Info* out) {
    Device d;
    if (!device_at(index, &d)) return false;
    out->back = index % kRolesPerDevice == 0;
    out->name = d.name;
    out->sizes.clear();
    for (const Format& f : d.formats) out->sizes.push_back({f.width, f.height, f.min_fps, f.max_fps});
    out->autofocus = (d.controls & kCtlAutofocus) != 0;
    out->torch = (d.controls & kCtlTorch) != 0;
    return true;
}

std::shared_ptr<Stream> open(int index) {
    if (!camera_allowed()) return nullptr;
    Device d;
    if (!device_at(index, &d) || d.formats.empty()) return nullptr;
    std::fprintf(stderr, "[camera] NDK open(%d) %s: %zu sizes\n", index, d.name.c_str(), d.formats.size());
    return std::make_shared<SessionStream>(std::make_shared<Session>(index, d.link, d.formats));
}

}  // namespace camera

HostMethods native_camera_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

}  // namespace winapk::host
