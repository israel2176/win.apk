// camera_host.h -- the PC's webcams for code that is not Java's: native_camera.cpp's devices and
// capture sessions (Media Foundation, NV12, full range) with no JNI type, for the NDK's camera
// (ndk_camera.cpp). Cameras are numbered as CameraBridge numbers them, each webcam twice (a back
// and a front camera), so that "0" names the same camera to camera2 in Java and to libcamera2ndk.
// test/ndk_camera_test.cpp stands in for all of it with a camera of its own.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace winapk::host::camera {

struct Size {
    int width = 0, height = 0;
    double min_fps = 0, max_fps = 0;
};

struct Info {
    bool back = true;          // the webcam's back role (even numbers) or its front one
    std::string name;
    std::vector<Size> sizes;   // distinct sizes, largest first
    bool autofocus = false, torch = false;
};

// One NV12 picture, full range: `height` rows of Y, then height/2 rows of interleaved CbCr, all
// `stride` bytes apart. `hold` keeps the bytes alive (the session reuses its buffers otherwise).
struct Picture {
    const uint8_t* nv12 = nullptr;
    int width = 0, height = 0, stride = 0;
    int64_t timestamp_ns = 0;  // the steady clock (QueryPerformanceCounter), as System.nanoTime
    std::shared_ptr<const void> hold;
};

// A capture session on one camera. Frames go to the listener on the capture thread, which must
// return quickly and must not stop the stream.
class Stream {
public:
    virtual ~Stream() = default;
    // The advertised size nearest the request, at a rate clamped into its range.
    virtual bool configure(int width, int height, int fps) = 0;
    virtual int width() = 0;
    virtual int height() = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual void set_listener(std::function<void(const Picture&)> listener) = 0;
    // Continuous autofocus (true) or the lens held; the torch on or off (false: no torch, or not
    // capturing).
    virtual void set_continuous_focus(bool on) = 0;
    virtual bool set_torch(bool on) = 0;
    // 0 unknown, 1 searching, 2 focused, 3 failed (native_camera.cpp's focus_state).
    virtual int focus_state() = 0;
};

// Whether Windows' privacy settings let a desktop program use the camera.
bool allowed();
// Whether the app holds android.permission.CAMERA (SpecialAccessStore, as AudioRecord asks for
// RECORD_AUDIO); true where nothing can be asked (no JVM).
bool permitted();
// How many cameras there are (none while allowed() is false).
int count();
bool info(int index, Info* out);
// Null when the camera does not exist or cannot be opened.
std::shared_ptr<Stream> open(int index);

}  // namespace winapk::host::camera
